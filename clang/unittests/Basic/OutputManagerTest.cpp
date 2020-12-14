//===- unittests/Basic/OutputManagerTest.cpp - OutputManager tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/OutputManager.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace llvm;

namespace {

struct OnDiskTempDirectory {
  bool Created = false;
  SmallString<128> Path;

  OnDiskTempDirectory() = delete;
  explicit OnDiskTempDirectory(StringRef Name) {
    if (!sys::fs::createUniqueDirectory(Name, Path))
      Created = true;
  }
  ~OnDiskTempDirectory() {
    if (Created)
      sys::fs::remove_directories(Path);
  }
};

struct OnDiskFile {
  const OnDiskTempDirectory &D;
  SmallString<128> Path;
  StringRef ParentPath;
  StringRef Filename;
  StringRef Stem;
  StringRef Extension;
  std::unique_ptr<MemoryBuffer> LastBuffer;

  template <class... PathArgTypes>
  OnDiskFile(const OnDiskTempDirectory &D, PathArgTypes &&... PathArgs) : D(D) {
    assert(D.Created);
    sys::path::append(Path, D.Path, std::forward<PathArgTypes>(PathArgs)...);
    ParentPath = sys::path::parent_path(Path);
    Filename = sys::path::filename(Path);
    Stem = sys::path::stem(Filename);
    Extension = sys::path::extension(Filename);
  }

  Optional<OnDiskFile> findTemp() const {
    std::error_code EC;
    for (sys::fs::directory_iterator I(ParentPath, EC), E; !EC && I != E;
         I.increment(EC)) {
      StringRef TempPath = I->path();
      if (!TempPath.startswith(D.Path))
        continue;

      // Look for "<stem>-*.<extension>.tmp".
      if (sys::path::extension(TempPath) != ".tmp")
        continue;

      // Drop the ".tmp" and check the extension and stem.
      StringRef TempStem = sys::path::stem(TempPath);
      if (sys::path::extension(TempStem) != Extension)
        continue;
      StringRef OriginalStem = sys::path::stem(TempStem);
      if (!OriginalStem.startswith(Stem))
        continue;
      if (!OriginalStem.drop_front(Stem.size()).startswith("-"))
        continue;

      // Found it.
      return OnDiskFile(D, TempPath.drop_front(D.Path.size() + 1));
    }
    return None;
  }

  Optional<sys::fs::UniqueID> getCurrentUniqueID() {
    sys::fs::file_status Status;
    sys::fs::status(Path, Status, /*follow=*/false);
    if (!sys::fs::is_regular_file(Status))
      return None;
    return Status.getUniqueID();
  }

  bool hasUniqueID(sys::fs::UniqueID ID) {
    auto CurrentID = getCurrentUniqueID();
    if (!CurrentID)
      return false;
    return *CurrentID == ID;
  }

  Optional<StringRef> getCurrentContent() {
    auto OnDiskOrErr = MemoryBuffer::getFile(Path);
    if (!OnDiskOrErr)
      return None;
    LastBuffer = std::move(*OnDiskOrErr);
    return LastBuffer->getBuffer();
  }

  bool equalsCurrentContent(StringRef Data) {
    auto CurrentContent = getCurrentContent();
    if (!CurrentContent)
      return false;
    return *CurrentContent == Data;
  }

  bool equalsCurrentContent(NoneType) { return getCurrentContent() == None; }
};

struct OutputManagerTest : public ::testing::Test {
  Optional<OutputManager> M; // Only optional to get a fresh one each time.
  Optional<OnDiskTempDirectory> D;

  void SetUp() override { M.emplace(); }
  void TearDown() override {
    M = None;
    D = None;
  }

  bool makeTempDirectory() {
    D.emplace("OutputManagerTest.d");
    return D->Created;
  }
};

TEST_F(OutputManagerTest, DefaultConfig) {
  // Check some important settings in the default configuration.
  EXPECT_FALSE(M->getDefaults().test(InMemoryOutputConfig::Enabled));
  EXPECT_TRUE(M->getDefaults().test(OnDiskOutputConfig::Enabled));
  EXPECT_TRUE(M->getDefaults().test(OnDiskOutputConfig::UseTemporary));
  EXPECT_TRUE(M->getDefaults().test(OnDiskOutputConfig::RemoveFileOnSignal));
}

TEST_F(OutputManagerTest, Ignored) {
  M->getDefaults().reset(
      {OnDiskOutputConfig::Enabled, InMemoryOutputConfig::Enabled});

  // FIXME: use native separators here and elsewhere?

  // Create the output.
  auto CreatedResult = expectedToOptional(M->createOutput("ignored.data"));
  ASSERT_TRUE(CreatedResult);
  EXPECT_TRUE(M->isNullHandle(CreatedResult->Handle));

  // Write some data into it and close the stream.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;

  // Closing should have no effect.
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));
}

TEST_F(OutputManagerTest, IgnoredErase) {
  M->getDefaults().reset(
      {OnDiskOutputConfig::Enabled, InMemoryOutputConfig::Enabled});

  // Create the output.
  auto CreatedResult = expectedToOptional(M->createOutput("ignored.data"));
  ASSERT_TRUE(CreatedResult);
  EXPECT_TRUE(M->isNullHandle(CreatedResult->Handle));

  // Write some data into it and close the stream.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;

  // Erasing should should do nothing.
  M->eraseOutput(CreatedResult->Handle);
}

TEST_F(OutputManagerTest, OnDisk) {
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::Enabled));
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::UseTemporary));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto CreatedResult = expectedToOptional(M->createOutput(File.Path));
  ASSERT_TRUE(CreatedResult);

  // File shouldn't exist, but the temporary should.
  EXPECT_TRUE(File.equalsCurrentContent(None));
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_TRUE(Temp);
  Optional<sys::fs::UniqueID> TempID = Temp->getCurrentUniqueID();
  ASSERT_TRUE(TempID);

  // Write some data into it and flush.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;
  EXPECT_TRUE(Temp->equalsCurrentContent("some data"));
  EXPECT_TRUE(File.equalsCurrentContent(None));

  // Close and check again.
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));
  EXPECT_TRUE(Temp->equalsCurrentContent(None));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));

  // The temp file should have been moved.
  EXPECT_TRUE(File.hasUniqueID(*TempID));
}

TEST_F(OutputManagerTest, OnDiskErase) {
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::Enabled));
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::UseTemporary));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto CreatedResult = expectedToOptional(M->createOutput(File.Path));
  ASSERT_TRUE(CreatedResult);

  // File shouldn't exist, but the temporary should.
  EXPECT_TRUE(File.equalsCurrentContent(None));
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_TRUE(Temp);

  // Write some data into it and flush.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;

  // Erase and check that the temp is gone.
  M->eraseOutput(CreatedResult->Handle);
  EXPECT_TRUE(Temp->equalsCurrentContent(None));
  EXPECT_TRUE(File.equalsCurrentContent(None));
}

TEST_F(OutputManagerTest, OnDiskCreateMissingDirectories) {
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::Enabled));
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::UseTemporary));
  M->getDefaults().set(
      OnDiskOutputConfig::UseTemporaryCreateMissingDirectories);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "missing", "on-disk.data");

  // Create the file.
  auto CreatedResult = expectedToOptional(M->createOutput(File.Path));
  ASSERT_TRUE(CreatedResult);

  // File shouldn't exist, but the temporary should.
  EXPECT_TRUE(File.equalsCurrentContent(None));
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_TRUE(Temp);
  Optional<sys::fs::UniqueID> TempID = Temp->getCurrentUniqueID();
  ASSERT_TRUE(TempID);

  // Write some data into it and flush.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;
  EXPECT_TRUE(Temp->equalsCurrentContent("some data"));
  EXPECT_TRUE(File.equalsCurrentContent(None));

  // Close and check again.
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));
  EXPECT_TRUE(Temp->equalsCurrentContent(None));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));

  // The temp file should have been moved.
  EXPECT_TRUE(File.hasUniqueID(*TempID));
}

TEST_F(OutputManagerTest, OnDiskNoCreateMissingDirectories) {
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::Enabled));
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::UseTemporary));
  M->getDefaults().reset(
      OnDiskOutputConfig::UseTemporaryCreateMissingDirectories);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "missing", "on-disk.data");

  // Fail to create the missing directory.
  auto CreatedResult = M->createOutput(File.Path);
  ASSERT_FALSE(CreatedResult);
  std::error_code EC = errorToErrorCode(CreatedResult.takeError());
  EXPECT_EQ(EC.value(), int(std::errc::no_such_file_or_directory));
}

TEST_F(OutputManagerTest, OnDiskNoTemporary) {
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::Enabled));
  M->getDefaults().reset(OnDiskOutputConfig::UseTemporary);
  ASSERT_FALSE(M->getDefaults().test(OnDiskOutputConfig::UseTemporary));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto CreatedResult = expectedToOptional(M->createOutput(File.Path));
  ASSERT_TRUE(CreatedResult);

  // File should exist with no temporary.
  EXPECT_TRUE(File.equalsCurrentContent(""));
  Optional<sys::fs::UniqueID> ID = File.getCurrentUniqueID();
  ASSERT_TRUE(ID);
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_FALSE(Temp);

  // Write some data into it and flush.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;
  EXPECT_TRUE(File.equalsCurrentContent("some data"));

  // Close and check again.
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));
  EXPECT_TRUE(File.hasUniqueID(*ID));
}

TEST_F(OutputManagerTest, OnDiskNoTemporaryErase) {
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::Enabled));
  M->getDefaults().reset(OnDiskOutputConfig::UseTemporary);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto CreatedResult = expectedToOptional(M->createOutput(File.Path));
  ASSERT_TRUE(CreatedResult);

  // File should exist with no temporary.
  EXPECT_TRUE(File.equalsCurrentContent(""));

  // Write some data into it and flush.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;
  EXPECT_TRUE(File.equalsCurrentContent("some data"));

  // Erase. Since UseTemporary is off the file should now be gone.
  M->eraseOutput(CreatedResult->Handle);
  EXPECT_TRUE(File.equalsCurrentContent(None));
}

TEST_F(OutputManagerTest, OnDiskWriteThroughBuffer) {
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::Enabled));
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::UseTemporary));
  M->getDefaults().set(OnDiskOutputConfig::UseWriteThroughWhenAlone);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto CreatedResult = expectedToOptional(M->createOutput(File.Path));
  ASSERT_TRUE(CreatedResult);

  // File shouldn't exist, but the temporary should.
  EXPECT_TRUE(File.equalsCurrentContent(None));
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_TRUE(Temp);
  Optional<sys::fs::UniqueID> TempID = Temp->getCurrentUniqueID();
  ASSERT_TRUE(TempID);

  // Write some data into it and flush. Confirm it's not on disk because it's
  // waiting to be written using a write-through buffer.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;
  EXPECT_TRUE(Temp->equalsCurrentContent(""));
  EXPECT_TRUE(File.equalsCurrentContent(None));

  // Close and check again.
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));
  EXPECT_TRUE(Temp->equalsCurrentContent(None));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));
  EXPECT_TRUE(File.hasUniqueID(*TempID));
}

TEST_F(OutputManagerTest, InMemory) {
  M->getDefaults()
      .reset(OnDiskOutputConfig::Enabled)
      .set(InMemoryOutputConfig::Enabled);

  IntrusiveRefCntPtr<vfs::InMemoryFileSystem> FS = new vfs::InMemoryFileSystem;
  M->setInMemoryFileSystem(FS);

  // Create the output.
  StringRef Path = "//root/in/memory.data";
  auto CreatedResult = expectedToOptional(M->createOutput(Path));
  ASSERT_TRUE(CreatedResult);

  // Write some data into it, flush, and close.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));

  // Lookup the file.
  auto FileOrErr = FS->openFileForRead(Path);
  ASSERT_TRUE(FileOrErr);
  ASSERT_TRUE(*FileOrErr);
  auto BufferOrErr = (*FileOrErr)->getBuffer(Path);
  ASSERT_TRUE(BufferOrErr);
  ASSERT_TRUE(*BufferOrErr);
  MemoryBuffer &Buffer = **BufferOrErr;
  EXPECT_EQ("some data", Buffer.getBuffer());
}

TEST_F(OutputManagerTest, requestOutputCallback) {
  M->getDefaults().reset(OnDiskOutputConfig::Enabled);

  int ClosedCount = 0;
  EXPECT_FALSE(errorToBool(
      M->requestOutputCallback("request.data", [&](StringRef Filename) {
        EXPECT_EQ("request.data", Filename);
        ++ClosedCount;
      })));
  auto CreatedResult = expectedToOptional(M->createOutput("request.data"));
  ASSERT_TRUE(CreatedResult);

  CreatedResult->OS = nullptr;

  // Check the callback is used.
  EXPECT_EQ(0, ClosedCount);
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));
  EXPECT_EQ(1, ClosedCount);
}

TEST_F(OutputManagerTest, requestOutputCallbackErased) {
  M->getDefaults().reset(OnDiskOutputConfig::Enabled);

  int ClosedCount = 0;
  EXPECT_FALSE(errorToBool(
      M->requestOutputCallback("request.data", [&](StringRef Filename) {
        EXPECT_EQ("request.data", Filename);
        ++ClosedCount;
      })));
  auto CreatedResult = expectedToOptional(M->createOutput("request.data"));
  ASSERT_TRUE(CreatedResult);

  CreatedResult->OS = nullptr;

  // Check the callback is used.
  EXPECT_EQ(0, ClosedCount);
  M->eraseOutput(CreatedResult->Handle);
  EXPECT_EQ(0, ClosedCount);
}

TEST_F(OutputManagerTest, requestOutput) {
  M->getDefaults().reset(OnDiskOutputConfig::Enabled);

  SmallString<128> Output;
  std::unique_ptr<raw_pwrite_stream> OS =
      std::make_unique<raw_svector_ostream>(Output);
  const raw_pwrite_stream *OriginalOS = OS.get();

  StringRef Path = "request.data";
  ASSERT_FALSE(errorToBool(M->requestOutput("request.data", std::move(OS))));
  auto CreatedResult = expectedToOptional(M->createOutput(Path));
  ASSERT_TRUE(CreatedResult);
  EXPECT_EQ(OriginalOS, CreatedResult->OS.get());

  // Write some data into it, which should show up right away.
  *CreatedResult->OS << "some data";
  EXPECT_EQ("some data", Output);

  // Ensure cleanup doesn't cause any issues.
  CreatedResult->OS = nullptr;
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));
  EXPECT_EQ("some data", Output);
}

TEST_F(OutputManagerTest, requestOutputWithCallback) {
  M->getDefaults().reset(OnDiskOutputConfig::Enabled);

  SmallString<128> Output;
  std::unique_ptr<raw_pwrite_stream> OS =
      std::make_unique<raw_svector_ostream>(Output);
  const raw_pwrite_stream *OriginalOS = OS.get();

  int ClosedCount = 0;
  EXPECT_FALSE(errorToBool(
      M->requestOutput("request.data", std::move(OS), [&](StringRef Filename) {
        EXPECT_EQ("request.data", Filename);
        ++ClosedCount;
      })));
  auto CreatedResult = expectedToOptional(M->createOutput("request.data"));
  ASSERT_TRUE(CreatedResult);
  EXPECT_EQ(OriginalOS, CreatedResult->OS.get());

  // Write some data into it, which should show up right away.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;
  EXPECT_EQ("some data", Output);

  // Check the callback is used.
  EXPECT_EQ(0, ClosedCount);
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));
  EXPECT_EQ(1, ClosedCount);
  EXPECT_EQ("some data", Output);
}

TEST_F(OutputManagerTest, requestOutputWithOnDisk) {
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::Enabled));
  ASSERT_TRUE(M->getDefaults().test(OnDiskOutputConfig::UseTemporary));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Request the output.
  SmallString<128> Output;
  std::unique_ptr<raw_pwrite_stream> OS =
      std::make_unique<raw_svector_ostream>(Output);
  ASSERT_FALSE(errorToBool(M->requestOutput(File.Path, std::move(OS))));

  // Create the file.
  auto CreatedResult = expectedToOptional(M->createOutput(File.Path));
  ASSERT_TRUE(CreatedResult);
  ASSERT_TRUE(File.findTemp());

  // Write some data and flush. The captured output won't be there yet.
  *CreatedResult->OS << "some data";
  CreatedResult->OS = nullptr;
  EXPECT_TRUE(Output.empty());

  // Close and check again.
  EXPECT_FALSE(errorToBool(M->closeOutput(CreatedResult->Handle)));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));
  EXPECT_EQ("some data", Output);
}

} // anonymous namespace
