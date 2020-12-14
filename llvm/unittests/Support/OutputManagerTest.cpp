//===- unittests/Support/OutputManagerTest.cpp - OutputManager tests ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/OutputManager.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::vfs;

namespace {

using ContentBuffer = OutputDestination::ContentBuffer;

TEST(OutputDestinationContentBufferTest, takeBuffer) {
  MemoryBufferRef B("data", "name");

  ContentBuffer Content = B;
  EXPECT_FALSE(Content.ownsContent());
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_EQ(B.getBufferStart(), Content.getBytes().begin());

  std::unique_ptr<MemoryBuffer> Taken = Content.takeBuffer();
  ASSERT_TRUE(Taken);
  EXPECT_EQ("data", Taken->getBuffer());
  EXPECT_EQ("name", Taken->getBufferIdentifier());

  // The buffer should be a reference to the original data, and Content should
  // still have it too.
  EXPECT_EQ(B.getBufferStart(), Taken->getBufferStart());
  EXPECT_EQ(Content.getBytes().begin(), Taken->getBufferStart());
}

TEST(OutputDestinationContentBufferTest, takeBufferVector) {
  SmallString<0> V = StringRef("data");
  StringRef R = V;

  ContentBuffer Content(std::move(V), "name");
  EXPECT_TRUE(Content.ownsContent());
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_EQ(R.begin(), Content.getBytes().begin());
  EXPECT_NE(V.begin(), Content.getBytes().begin());

  std::unique_ptr<MemoryBuffer> Taken = Content.takeBuffer();
  ASSERT_TRUE(Taken);
  EXPECT_EQ("data", Taken->getBuffer());
  EXPECT_EQ("name", Taken->getBufferIdentifier());
  EXPECT_EQ(R.begin(), Taken->getBufferStart());

  // Content should still have a reference to the data.
  EXPECT_FALSE(Content.ownsContent());
  EXPECT_EQ(Taken->getBuffer(), Content.getBytes());
  EXPECT_EQ(Taken->getBufferStart(), Content.getBytes().begin());
  EXPECT_EQ(Content.getBytes().begin(), Taken->getBufferStart());
}

TEST(OutputDestinationContentBufferTest, takeBufferVectorSmallStorage) {
  // Something using small storage will have to move.
  SmallString<128> V = StringRef("data");
  ASSERT_EQ(128u, V.capacity());
  StringRef R = V;

  ContentBuffer Content(std::move(V), "name");
  EXPECT_TRUE(Content.ownsContent());
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_NE(R.begin(), Content.getBytes().begin());
}

TEST(OutputDestinationContentBufferTest, takeBufferMemoryBuffer) {
  std::unique_ptr<MemoryBuffer> B = MemoryBuffer::getMemBuffer("data", "name");
  MemoryBufferRef R = *B;

  ContentBuffer Content = std::move(B);
  EXPECT_TRUE(Content.ownsContent());
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_EQ(R.getBufferStart(), Content.getBytes().begin());

  std::unique_ptr<MemoryBuffer> Taken = Content.takeBuffer();
  ASSERT_TRUE(Taken);
  EXPECT_EQ("data", Taken->getBuffer());
  EXPECT_EQ("name", Taken->getBufferIdentifier());
  EXPECT_EQ(R.getBufferStart(), Taken->getBufferStart());

  // Content should still have a reference to the data.
  EXPECT_FALSE(Content.ownsContent());
  EXPECT_EQ(Taken->getBuffer(), Content.getBytes());
  EXPECT_EQ(Taken->getBufferStart(), Content.getBytes().begin());
  EXPECT_EQ(Content.getBytes().begin(), Taken->getBufferStart());
}

TEST(OutputDestinationContentBufferTest, takeOwnedBufferOrNull) {
  MemoryBufferRef B("data", "name");

  ContentBuffer Content = B;
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_EQ(B.getBufferStart(), Content.getBytes().begin());

  // ContentBuffer doesn't own this.
  EXPECT_FALSE(Content.takeOwnedBufferOrNull());
}

TEST(OutputDestinationContentBufferTest, takeOwnedBufferOrNullVector) {
  SmallString<0> V = StringRef("data");
  StringRef R = V;

  ContentBuffer Content(std::move(V), "name");
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_EQ(R.begin(), Content.getBytes().begin());
  EXPECT_NE(V.begin(), Content.getBytes().begin());

  std::unique_ptr<MemoryBuffer> Taken = Content.takeOwnedBufferOrNull();
  ASSERT_TRUE(Taken);
  EXPECT_EQ("data", Taken->getBuffer());
  EXPECT_EQ("name", Taken->getBufferIdentifier());
  EXPECT_EQ(R.begin(), Taken->getBufferStart());

  // Content should still have a reference to the data.
  EXPECT_FALSE(Content.ownsContent());
  EXPECT_EQ(Taken->getBuffer(), Content.getBytes());
  EXPECT_EQ(Taken->getBufferStart(), Content.getBytes().begin());
  EXPECT_EQ(Content.getBytes().begin(), Taken->getBufferStart());

  // The next call should fail since it's no longer owned.
  EXPECT_FALSE(Content.takeOwnedBufferOrNull());
}

TEST(OutputDestinationContentBufferTest, takeOwnedBufferOrNullVectorEmpty) {
  SmallString<0> V;

  ContentBuffer Content(std::move(V), "name");
  EXPECT_EQ("", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());

  // Check that ContentBuffer::takeOwnedBufferOrNull() fails for an empty
  // vector.
  EXPECT_FALSE(Content.ownsContent());
  ASSERT_FALSE(Content.takeOwnedBufferOrNull());

  // ContentBuffer::takeBuffer() should still work.
  std::unique_ptr<MemoryBuffer> Taken = Content.takeBuffer();
  ASSERT_TRUE(Taken);
  EXPECT_EQ("", Taken->getBuffer());
  EXPECT_EQ("name", Taken->getBufferIdentifier());
}

TEST(OutputDestinationContentBufferTest, takeOwnedBufferOrNullMemoryBuffer) {
  std::unique_ptr<MemoryBuffer> B = MemoryBuffer::getMemBuffer("data", "name");
  MemoryBufferRef R = *B;

  ContentBuffer Content = std::move(B);
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_EQ(R.getBufferStart(), Content.getBytes().begin());

  std::unique_ptr<MemoryBuffer> Taken = Content.takeOwnedBufferOrNull();
  ASSERT_TRUE(Taken);
  EXPECT_EQ("data", Taken->getBuffer());
  EXPECT_EQ("name", Taken->getBufferIdentifier());
  EXPECT_EQ(R.getBufferStart(), Taken->getBufferStart());

  // Content should still have a reference to the data.
  EXPECT_EQ(Taken->getBuffer(), Content.getBytes());
  EXPECT_EQ(Taken->getBufferStart(), Content.getBytes().begin());
  EXPECT_EQ(Content.getBytes().begin(), Taken->getBufferStart());

  // The next call should fail since it's no longer owned.
  EXPECT_FALSE(Content.takeOwnedBufferOrNull());
}

TEST(OutputDestinationContentBufferTest, takeOwnedBufferOrCopy) {
  MemoryBufferRef B("data", "name");

  ContentBuffer Content = B;
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_EQ(B.getBufferStart(), Content.getBytes().begin());

  std::unique_ptr<MemoryBuffer> Taken = Content.takeOwnedBufferOrCopy();
  ASSERT_TRUE(Taken);
  EXPECT_EQ("data", Taken->getBuffer());
  EXPECT_EQ("name", Taken->getBufferIdentifier());

  // We should have a copy, but Content should be unchanged.
  EXPECT_NE(B.getBufferStart(), Taken->getBufferStart());
  EXPECT_EQ(B.getBufferStart(), Content.getBytes().begin());
}

TEST(OutputDestinationContentBufferTest, takeOwnedBufferOrCopyVector) {
  SmallString<0> V = StringRef("data");
  StringRef R = V;

  ContentBuffer Content(std::move(V), "name");
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_EQ(R.begin(), Content.getBytes().begin());
  EXPECT_NE(V.begin(), Content.getBytes().begin());

  std::unique_ptr<MemoryBuffer> Taken = Content.takeOwnedBufferOrCopy();
  ASSERT_TRUE(Taken);
  EXPECT_EQ("data", Taken->getBuffer());
  EXPECT_EQ("name", Taken->getBufferIdentifier());

  // Content should still have a reference to the data, but it's no longer
  // owned.
  EXPECT_EQ(Taken->getBuffer(), Content.getBytes());
  EXPECT_EQ(Taken->getBufferStart(), Content.getBytes().begin());
  EXPECT_EQ(Content.getBytes().begin(), Taken->getBufferStart());
  EXPECT_FALSE(Content.takeOwnedBufferOrNull());

  // The next call should get a different (but equal) buffer.
  std::unique_ptr<MemoryBuffer> Copy = Content.takeOwnedBufferOrCopy();
  ASSERT_TRUE(Copy);
  EXPECT_EQ("data", Copy->getBuffer());
  EXPECT_EQ("name", Copy->getBufferIdentifier());
  EXPECT_NE(Taken->getBufferStart(), Copy->getBufferStart());
}

TEST(OutputDestinationContentBufferTest, takeOwnedBufferOrCopyMemoryBuffer) {
  std::unique_ptr<MemoryBuffer> B = MemoryBuffer::getMemBuffer("data", "name");
  MemoryBufferRef R = *B;

  ContentBuffer Content = std::move(B);
  EXPECT_EQ("data", Content.getBytes());
  EXPECT_EQ("name", Content.getPath());
  EXPECT_EQ(R.getBufferStart(), Content.getBytes().begin());

  std::unique_ptr<MemoryBuffer> Taken = Content.takeOwnedBufferOrCopy();
  ASSERT_TRUE(Taken);
  EXPECT_EQ("data", Taken->getBuffer());
  EXPECT_EQ("name", Taken->getBufferIdentifier());
  EXPECT_EQ(R.getBufferStart(), Taken->getBufferStart());

  // Content should still have a reference to the data.
  EXPECT_EQ(Taken->getBuffer(), Content.getBytes());
  EXPECT_EQ(Taken->getBufferStart(), Content.getBytes().begin());
  EXPECT_EQ(Content.getBytes().begin(), Taken->getBufferStart());

  // The next call should get a different (but equal) buffer.
  std::unique_ptr<MemoryBuffer> Copy = Content.takeOwnedBufferOrCopy();
  ASSERT_TRUE(Copy);
  EXPECT_EQ("data", Copy->getBuffer());
  EXPECT_EQ("name", Copy->getBufferIdentifier());
  EXPECT_NE(Taken->getBufferStart(), Copy->getBufferStart());
}

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
  Optional<OnDiskTempDirectory> D;

  void TearDown() override { D = None; }

  bool makeTempDirectory() {
    D.emplace("OutputManagerTest.d");
    return D->Created;
  }

  template <class T>
  std::unique_ptr<T> expectedToPointer(Expected<std::unique_ptr<T>> Expected) {
    if (auto Maybe = expectedToOptional(std::move(Expected)))
      return std::move(*Maybe);
    return nullptr;
  }

  std::unique_ptr<MemoryBuffer> openBufferForRead(InMemoryFileSystem &FS,
                                                  StringRef Path) {
    if (auto FileOrErr = FS.openFileForRead(Path))
      if (*FileOrErr)
        if (auto BufferOrErr = (*FileOrErr)->getBuffer(Path))
          if (*BufferOrErr)
            return std::move(*BufferOrErr);
    return nullptr;
  }
};

TEST_F(OutputManagerTest, DefaultConfig) {
  OutputManager M;

  // Check some important settings in the default configuration.
  EXPECT_TRUE(M.getDefaults().test(ClientIntentOutputConfig::NeedsSeeking));
  EXPECT_TRUE(M.getDefaults().test(OnDiskOutputConfig::UseTemporary));
  EXPECT_FALSE(M.getDefaults().test(
      OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
  EXPECT_TRUE(M.getDefaults().test(OnDiskOutputConfig::RemoveFileOnSignal));
}

TEST_F(OutputManagerTest, Null) {
  OutputManager M(makeNullOutputBackend());

  // Create the output.
  auto O = expectedToPointer(M.createOutput("ignored.data"));
  ASSERT_TRUE(O);
  ASSERT_TRUE(O->getOS());

  // Write some data into it and close the stream.
  *O->getOS() << "some data";
  (void)O->takeOS();

  // Closing should have no effect.
  EXPECT_FALSE(errorToBool(O->close()));
}

TEST_F(OutputManagerTest, NullErase) {
  OutputManager M(makeNullOutputBackend());

  // Create the output.
  auto O = expectedToPointer(M.createOutput("ignored.data"));
  ASSERT_TRUE(O);
  ASSERT_TRUE(O->getOS());

  // Write some data into it and close the stream.
  *O->takeOS() << "some data";

  // Erasing should have no effect.
  O->erase();
}

TEST_F(OutputManagerTest, OnDisk) {
  OutputManager M;
  ASSERT_TRUE(M.getDefaults().test(OnDiskOutputConfig::UseTemporary));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // File shouldn't exist, but the temporary should.
  EXPECT_TRUE(File.equalsCurrentContent(None));
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_TRUE(Temp);
  Optional<sys::fs::UniqueID> TempID = Temp->getCurrentUniqueID();
  ASSERT_TRUE(TempID);

  // Write some data into it and flush.
  *O->takeOS() << "some data";
  EXPECT_TRUE(Temp->equalsCurrentContent("some data"));
  EXPECT_TRUE(File.equalsCurrentContent(None));

  // Close and check again.
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(Temp->equalsCurrentContent(None));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));

  // The temp file should have been moved.
  EXPECT_TRUE(File.hasUniqueID(*TempID));
}

TEST_F(OutputManagerTest, OnDiskErase) {
  OutputManager M;
  ASSERT_TRUE(M.getDefaults().test(OnDiskOutputConfig::UseTemporary));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // File shouldn't exist, but the temporary should.
  EXPECT_TRUE(File.equalsCurrentContent(None));
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_TRUE(Temp);

  // Write some data into it and close the stream.
  *O->takeOS() << "some data";

  // Erase and check that the temp is gone.
  O->erase();
  EXPECT_TRUE(Temp->equalsCurrentContent(None));
  EXPECT_TRUE(File.equalsCurrentContent(None));
}

TEST_F(OutputManagerTest, OnDiskCreateMissingDirectories) {
  OutputManager M;
  ASSERT_TRUE(M.getDefaults().test(OnDiskOutputConfig::UseTemporary));
  M.getDefaults().set(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "missing", "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // File shouldn't exist, but the temporary should.
  EXPECT_TRUE(File.equalsCurrentContent(None));
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_TRUE(Temp);
  Optional<sys::fs::UniqueID> TempID = Temp->getCurrentUniqueID();
  ASSERT_TRUE(TempID);

  // Write some data into it and flush.
  *O->takeOS() << "some data";
  EXPECT_TRUE(Temp->equalsCurrentContent("some data"));
  EXPECT_TRUE(File.equalsCurrentContent(None));

  // Close and check again.
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(Temp->equalsCurrentContent(None));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));

  // The temp file should have been moved.
  EXPECT_TRUE(File.hasUniqueID(*TempID));
}

TEST_F(OutputManagerTest, OnDiskNoCreateMissingDirectories) {
  OutputManager M;
  ASSERT_TRUE(M.getDefaults().test(OnDiskOutputConfig::UseTemporary));
  M.getDefaults().reset(
      OnDiskOutputConfig::UseTemporaryCreateMissingDirectories);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "missing", "on-disk.data");

  // Fail to create the missing directory.
  auto Expected = M.createOutput(File.Path);
  ASSERT_FALSE(Expected);
  std::error_code EC = errorToErrorCode(Expected.takeError());
  EXPECT_EQ(int(std::errc::no_such_file_or_directory), EC.value());
}

TEST_F(OutputManagerTest, OnDiskNoTemporary) {
  OutputManager M;
  M.getDefaults().reset(OnDiskOutputConfig::UseTemporary);
  ASSERT_FALSE(M.getDefaults().test(OnDiskOutputConfig::UseTemporary));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // File should exist with no temporary.
  EXPECT_TRUE(File.equalsCurrentContent(""));
  Optional<sys::fs::UniqueID> ID = File.getCurrentUniqueID();
  ASSERT_TRUE(ID);
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_FALSE(Temp);

  // Write some data into it and flush.
  *O->takeOS() << "some data";
  EXPECT_TRUE(File.equalsCurrentContent("some data"));

  // Close and check again.
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));
  EXPECT_TRUE(File.hasUniqueID(*ID));
}

TEST_F(OutputManagerTest, OnDiskNoTemporaryErase) {
  OutputManager M;
  M.getDefaults().reset(OnDiskOutputConfig::UseTemporary);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // File should exist with no temporary.
  EXPECT_TRUE(File.equalsCurrentContent(""));

  // Write some data into it and flush.
  *O->takeOS() << "some data";
  EXPECT_TRUE(File.equalsCurrentContent("some data"));

  // Erase. Since UseTemporary is off the file should now be gone.
  O->erase();
  EXPECT_TRUE(File.equalsCurrentContent(None));
}

TEST_F(OutputManagerTest, OnDiskWriteThroughBuffer) {
  OutputManager M;
  ASSERT_TRUE(M.getDefaults().test(OnDiskOutputConfig::UseTemporary));
  M.getDefaults().set(OnDiskOutputConfig::UseWriteThroughBuffer);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // File shouldn't exist, but the temporary should.
  EXPECT_TRUE(File.equalsCurrentContent(None));
  Optional<OnDiskFile> Temp = File.findTemp();
  ASSERT_TRUE(Temp);
  Optional<sys::fs::UniqueID> TempID = Temp->getCurrentUniqueID();
  ASSERT_TRUE(TempID);

  // Write some data into it and flush. Confirm it's not on disk because it's
  // waiting to be written using a write-through buffer.
  *O->takeOS() << "some data";
  EXPECT_TRUE(Temp->equalsCurrentContent(""));
  EXPECT_TRUE(File.equalsCurrentContent(None));

  // Close and check again.
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(Temp->equalsCurrentContent(None));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));
  EXPECT_TRUE(File.hasUniqueID(*TempID));
}

struct CaptureLastBufferOutputBackend : public OutputBackend {
  struct Destination : public OutputDestination {
    Error storeContentImpl(ContentBuffer &Content) final {
      Storage = Content.takeBuffer();
      return Error::success();
    }

    Destination(std::unique_ptr<MemoryBuffer> &Storage,
                std::unique_ptr<OutputDestination> NextDest)
        : OutputDestination(std::move(NextDest)), Storage(Storage) {}

    std::unique_ptr<MemoryBuffer> &Storage;
  };

  Expected<std::unique_ptr<OutputDestination>>
  createDestination(StringRef OutputPath, OutputConfig Config,
                    std::unique_ptr<OutputDestination> NextDest) final {
    return std::make_unique<Destination>(Storage, std::move(NextDest));
  }

  std::unique_ptr<MemoryBuffer> &Storage;
  CaptureLastBufferOutputBackend(std::unique_ptr<MemoryBuffer> &Storage)
      : Storage(Storage) {}
};

TEST_F(OutputManagerTest, OnDiskWriteThroughBufferReturned) {
  std::unique_ptr<MemoryBuffer> Buffer;
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      makeIntrusiveRefCnt<CaptureLastBufferOutputBackend>(Buffer)));
  M.getDefaults().set(OnDiskOutputConfig::UseWriteThroughBuffer);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // Write enough data to get a write-through buffer, and add one to ensure
  // it's not page-aligned.
  std::string Data;
  Data.append(OnDiskOutputBackend::MinimumSizeToReturnWriteThroughBuffer + 1,
              'z');
  *O->takeOS() << Data;
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(File.equalsCurrentContent(Data));

  // Check that the saved buffer is a write-through buffer.
  EXPECT_TRUE(Buffer);
  EXPECT_EQ(File.Path, Buffer->getBufferIdentifier());
  EXPECT_EQ(Data, Buffer->getBuffer());
  EXPECT_EQ(MemoryBuffer::MemoryBuffer_MMap, Buffer->getBufferKind());
}

TEST_F(OutputManagerTest, OnDiskWriteThroughBufferNotReturned) {
  std::unique_ptr<MemoryBuffer> Buffer;
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      makeIntrusiveRefCnt<CaptureLastBufferOutputBackend>(Buffer)));
  M.getDefaults().set(OnDiskOutputConfig::UseWriteThroughBuffer);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // Write almost enough data to return write-through buffer, but not quite.
  std::string Data;
  Data.append(OnDiskOutputBackend::MinimumSizeToReturnWriteThroughBuffer - 1,
              'z');
  *O->takeOS() << Data;
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(File.equalsCurrentContent(Data));

  // Check that the saved buffer is not write-through buffer.
  EXPECT_TRUE(Buffer);
  EXPECT_EQ(File.Path, Buffer->getBufferIdentifier());
  EXPECT_EQ(Data, Buffer->getBuffer());
  EXPECT_EQ(MemoryBuffer::MemoryBuffer_Malloc, Buffer->getBufferKind());
}

TEST_F(OutputManagerTest, FilteredOnDisk) {
  OutputManager M(
      makeFilteringOutputBackend(makeIntrusiveRefCnt<OnDiskOutputBackend>(),
                                 [](StringRef, OutputConfig) { return true; }));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close. The content should be there.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));
}

TEST_F(OutputManagerTest, FilteredOnDiskSkipped) {
  OutputManager M(makeFilteringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      [](StringRef, OutputConfig) { return false; }));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close. It should not exist on disk.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(File.equalsCurrentContent(None));
}
TEST_F(OutputManagerTest, InMemory) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeIntrusiveRefCnt<InMemoryOutputBackend>(FS));

  // Create the output.
  StringRef Path = "//root/in/memory.data";
  ASSERT_FALSE(FS->exists(Path));
  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close.
  *O->takeOS() << "some data";
  ASSERT_FALSE(FS->exists(Path));
  EXPECT_FALSE(errorToBool(O->close()));

  // Lookup the file.
  ASSERT_TRUE(FS->exists(Path));
  std::unique_ptr<MemoryBuffer> Buffer = openBufferForRead(*FS, Path);
  EXPECT_EQ(Path, Buffer->getBufferIdentifier());
  EXPECT_EQ("some data", Buffer->getBuffer());
}

TEST_F(OutputManagerTest, InMemoryAlreadyExists) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeIntrusiveRefCnt<InMemoryOutputBackend>(FS));

  // Add a conflicting file to FS.
  StringRef Path = "//root/in/memory.data";
  FS->addFile(Path, 0, MemoryBuffer::getMemBuffer("some data"));

  // Check that we get the error from failing to clobber the in-memory file.
  auto Expected = M.createOutput(Path);
  ASSERT_FALSE(Expected);
  std::error_code EC = errorToErrorCode(Expected.takeError());
  EXPECT_EQ(int(std::errc::device_or_resource_busy), EC.value());
}

TEST_F(OutputManagerTest, InMemoryAlreadyExistsOnClose) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeIntrusiveRefCnt<InMemoryOutputBackend>(FS));

  // Create the output.
  StringRef Path = "//root/in/memory.data";
  ASSERT_FALSE(FS->exists(Path));
  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_FALSE(FS->exists(Path));
  ASSERT_TRUE(O);

  // Add a conflicting file to FS.
  FS->addFile(Path, 0, MemoryBuffer::getMemBuffer("old data"));
  ASSERT_TRUE(FS->exists(Path));

  // Write some data into it, flush, and close.
  *O->takeOS() << "new data";
  Error E = O->close();

  // Check the error.
  ASSERT_TRUE(bool(E));
  std::error_code EC = errorToErrorCode(std::move(E));
  EXPECT_EQ(int(std::errc::device_or_resource_busy), EC.value());
}

TEST_F(OutputManagerTest, MirrorOnDiskInMemory) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS)));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(File.equalsCurrentContent("some data"));

  // Lookup the file in FS.
  std::unique_ptr<MemoryBuffer> Buffer = openBufferForRead(*FS, File.Path);
  EXPECT_EQ(File.Path, Buffer->getBufferIdentifier());
  EXPECT_EQ("some data", Buffer->getBuffer());
}

TEST_F(OutputManagerTest, MirrorOnDiskInMemoryAlreadyExists) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS)));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Add a file with the same name to FS.
  FS->addFile(File.Path, 0, MemoryBuffer::getMemBuffer("some data"));

  // Check that we get the error from failing to clobber the in-memory file.
  auto Expected = M.createOutput(File.Path);
  ASSERT_FALSE(Expected);
  std::error_code EC = errorToErrorCode(Expected.takeError());
  EXPECT_EQ(int(std::errc::device_or_resource_busy), EC.value());
}

TEST_F(OutputManagerTest, MirrorOnDiskInMemoryAlreadyExistsOnClose) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS)));

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the output.
  ASSERT_FALSE(FS->exists(File.Path));
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_FALSE(FS->exists(File.Path));
  ASSERT_TRUE(O);

  // Add a conflicting file to FS.
  FS->addFile(File.Path, 0, MemoryBuffer::getMemBuffer("old data"));
  ASSERT_TRUE(FS->exists(File.Path));

  // Write some data into it, flush, and close.
  *O->takeOS() << "new data";
  Error E = O->close();

  // Check the error.
  ASSERT_TRUE(bool(E));
  std::error_code EC = errorToErrorCode(std::move(E));
  EXPECT_EQ(int(std::errc::device_or_resource_busy), EC.value());
}

TEST_F(OutputManagerTest, MirrorOnDiskInMemoryNoCreateMissingDirectories) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS)));
  M.getDefaults().reset(
      OnDiskOutputConfig::UseTemporaryCreateMissingDirectories);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "missing", "on-disk.data");

  // Check that we get the error from failing to create the missing directory
  // on-disk.
  auto Expected = M.createOutput(File.Path);
  ASSERT_FALSE(Expected);
  std::error_code EC = errorToErrorCode(Expected.takeError());
  EXPECT_EQ(int(std::errc::no_such_file_or_directory), EC.value());
}

TEST_F(OutputManagerTest, MirrorOnDiskInMemoryWriteThrough) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS)));
  M.getDefaults().set(OnDiskOutputConfig::UseWriteThroughBuffer);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // Write enough data to get a write-through buffer, and add one to ensure
  // it's not page-aligned.
  std::string Data;
  Data.append(OnDiskOutputBackend::MinimumSizeToReturnWriteThroughBuffer + 1,
              'z');
  *O->takeOS() << Data;
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(File.equalsCurrentContent(Data));

  // Lookup the file in FS.
  std::unique_ptr<MemoryBuffer> Buffer = openBufferForRead(*FS, File.Path);
  EXPECT_EQ(File.Path, Buffer->getBufferIdentifier());
  EXPECT_EQ(Data, Buffer->getBuffer());
}

TEST_F(OutputManagerTest, MirrorOnDiskInMemoryWriteThroughPageAligned) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS)));
  M.getDefaults().set(OnDiskOutputConfig::UseWriteThroughBuffer);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // Write just enough data to get a write-through buffer. It'll likely be
  // page-aligned.
  std::string Data;
  Data.append(OnDiskOutputBackend::MinimumSizeToReturnWriteThroughBuffer, 'z');
  *O->takeOS() << Data;
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(File.equalsCurrentContent(Data));

  // Lookup the file in FS. Ensure no errors when it creates a null-terminated
  // reference to the buffer.
  std::unique_ptr<MemoryBuffer> Buffer = openBufferForRead(*FS, File.Path);
  EXPECT_EQ(File.Path, Buffer->getBufferIdentifier());
  EXPECT_EQ(Data, Buffer->getBuffer());
}

TEST_F(OutputManagerTest, MirrorOnDiskInMemoryWriteThroughTooSmall) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<OnDiskOutputBackend>(),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS)));
  M.getDefaults().set(OnDiskOutputConfig::UseWriteThroughBuffer);

  // Get a filename we can use on disk.
  ASSERT_TRUE(makeTempDirectory());
  OnDiskFile File(*D, "on-disk.data");

  // Create the file.
  auto O = expectedToPointer(M.createOutput(File.Path));
  ASSERT_TRUE(O);

  // Write almost (but not quite) enough data to get a write-through buffer.
  std::string Data;
  Data.append(OnDiskOutputBackend::MinimumSizeToReturnWriteThroughBuffer - 1,
              'z');
  *O->takeOS() << Data;
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_TRUE(File.equalsCurrentContent(Data));

  // Lookup the file in FS.
  std::unique_ptr<MemoryBuffer> Buffer = openBufferForRead(*FS, File.Path);
  EXPECT_EQ(File.Path, Buffer->getBufferIdentifier());
  EXPECT_EQ(Data, Buffer->getBuffer());
}

TEST_F(OutputManagerTest, FilteredInMemory) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(
      makeFilteringOutputBackend(makeIntrusiveRefCnt<InMemoryOutputBackend>(FS),
                                 [](StringRef, OutputConfig) { return true; }));

  // Create the output.
  StringRef Path = "//root/in/memory.data";
  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));

  // Lookup the file.
  ASSERT_TRUE(FS->exists(Path));
  std::unique_ptr<MemoryBuffer> Buffer = openBufferForRead(*FS, Path);
  EXPECT_EQ(Path, Buffer->getBufferIdentifier());
  EXPECT_EQ("some data", Buffer->getBuffer());
}

TEST_F(OutputManagerTest, FilteredInMemorySkipped) {
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeFilteringOutputBackend(
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS),
      [](StringRef, OutputConfig) { return false; }));

  // Create the output.
  StringRef Path = "//root/in/memory.data";
  ASSERT_FALSE(FS->exists(Path));
  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));

  // The file should have been filtered out.
  ASSERT_FALSE(FS->exists(Path));
}

TEST_F(OutputManagerTest, MirrorFilteredInMemoryInMemoryNotFiltered) {
  auto FS1 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  auto FS2 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeFilteringOutputBackend(
          makeIntrusiveRefCnt<InMemoryOutputBackend>(FS1),
          [](StringRef, OutputConfig) { return false; }),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS2)));

  // Create the output.
  StringRef Path = "//root/in/memory.data";
  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));

  // Lookup the file. It should only be skipped in FS1.
  EXPECT_TRUE(!FS1->exists(Path));
  ASSERT_TRUE(FS2->exists(Path));
  std::unique_ptr<MemoryBuffer> Buffer2 = openBufferForRead(*FS2, Path);
  ASSERT_TRUE(Buffer2);
  EXPECT_EQ(Path, Buffer2->getBufferIdentifier());
  EXPECT_EQ("some data", Buffer2->getBuffer());
}

TEST_F(OutputManagerTest, MirrorInMemoryWithFilteredInMemory) {
  auto FS1 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  auto FS2 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS1),
      makeFilteringOutputBackend(
          makeIntrusiveRefCnt<InMemoryOutputBackend>(FS2),
          [](StringRef, OutputConfig) { return false; })));

  // Create the output.
  StringRef Path = "//root/in/memory.data";
  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));

  // Lookup the file. It should only be skipped in FS1.
  EXPECT_TRUE(!FS2->exists(Path));
  ASSERT_TRUE(FS1->exists(Path));
  std::unique_ptr<MemoryBuffer> Buffer2 = openBufferForRead(*FS1, Path);
  ASSERT_TRUE(Buffer2);
  EXPECT_EQ(Path, Buffer2->getBufferIdentifier());
  EXPECT_EQ("some data", Buffer2->getBuffer());
}

TEST_F(OutputManagerTest, CallbackStream) {
  SmallString<128> Output;
  std::unique_ptr<raw_pwrite_stream> OS =
      std::make_unique<raw_svector_ostream>(Output);
  const raw_pwrite_stream *OriginalOS = OS.get();

  StringRef Path = "request.data";
  struct {
    int Open = 0;
    int Close = 0;
    int Erase = 0;
  } Expected, Other;
  OutputManager M(makeCallbackStreamOutputBackend(
      [&](StringRef OutputPath,
          OutputConfig) -> std::unique_ptr<raw_pwrite_stream> {
        if (Path != OutputPath) {
          ++Other.Open;
          return nullptr;
        }
        ++Expected.Open;
        return std::move(OS);
      },
      [&](StringRef OutputPath) {
        if (OutputPath == Path)
          ++Expected.Close;
        else
          ++Other.Close;
        return Error::success();
      },
      [&](StringRef OutputPath) {
        if (OutputPath == Path)
          ++Expected.Erase;
        else
          ++Other.Erase;
      }));

  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);
  EXPECT_EQ(OriginalOS, O->getOS());
  EXPECT_EQ(0, Other.Open);
  EXPECT_EQ(1, Expected.Open);

  // Other open calls should return a valid Output, even though there's no
  // stream.
  EXPECT_TRUE(expectedToOptional(M.createOutput("unrequested.data")));
  EXPECT_EQ(1, Other.Open);
  EXPECT_EQ(1, Other.Erase);

  // Write some data into it, which should show up right away.
  *O->getOS() << "some data";
  EXPECT_EQ("some data", Output);

  // Ensure cleanup doesn't cause any issues.
  (void)O->takeOS();
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_EQ("some data", Output);
  EXPECT_EQ(1, Expected.Close);
  EXPECT_EQ(0, Expected.Erase);
  EXPECT_EQ(0, Other.Close);
}

TEST_F(OutputManagerTest, CallbackStreamErase) {
  SmallString<128> Output;
  std::unique_ptr<raw_pwrite_stream> OS =
      std::make_unique<raw_svector_ostream>(Output);
  const raw_pwrite_stream *OriginalOS = OS.get();

  StringRef Path = "request.data";
  struct {
    int Open = 0;
    int Close = 0;
    int Erase = 0;
  } Expected, Other;
  OutputManager M(makeCallbackStreamOutputBackend(
      [&](StringRef OutputPath,
          OutputConfig) -> std::unique_ptr<raw_pwrite_stream> {
        if (Path != OutputPath) {
          ++Other.Open;
          return nullptr;
        }
        ++Expected.Open;
        return std::move(OS);
      },
      [&](StringRef OutputPath) {
        if (OutputPath == Path)
          ++Expected.Close;
        else
          ++Other.Close;
        return Error::success();
      },
      [&](StringRef OutputPath) {
        if (OutputPath == Path)
          ++Expected.Erase;
        else
          ++Other.Erase;
      }));

  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);
  EXPECT_EQ(OriginalOS, O->getOS());
  EXPECT_EQ(0, Other.Open);
  EXPECT_EQ(1, Expected.Open);

  // Write some data into it, which should show up right away.
  *O->getOS() << "some data";
  EXPECT_EQ("some data", Output);

  // Ensure cleanup doesn't cause any issues.
  (void)O->takeOS();
  O->erase();
  EXPECT_EQ(1, Expected.Erase);
  EXPECT_EQ(0, Expected.Close);
  EXPECT_EQ(0, Other.Close);
  EXPECT_EQ(0, Other.Erase);
}

TEST_F(OutputManagerTest, EraseCalledOnOutputDestruction) {
  int Close = 0;
  int Erase = 0;
  OutputManager M(makeCallbackStreamOutputBackend(
      [&](StringRef OutputPath, OutputConfig)
          -> std::unique_ptr<raw_pwrite_stream> { return nullptr; },
      [&](StringRef OutputPath) {
        ++Close;
        return Error::success();
      },
      [&](StringRef OutputPath) { ++Erase; }));

  auto O = expectedToPointer(M.createOutput("request.data"));
  ASSERT_TRUE(O);
  EXPECT_EQ(0, Close);
  EXPECT_EQ(0, Erase);

  // Check that erase is called on destruction.
  O = nullptr;
  EXPECT_EQ(1, Erase);
  EXPECT_EQ(0, Close);
}

TEST_F(OutputManagerTest, MirrorCallbackStreamInMemory) {
  SmallString<128> Output;
  std::unique_ptr<raw_pwrite_stream> OS =
      std::make_unique<raw_svector_ostream>(Output);
  const raw_pwrite_stream *OriginalOS = OS.get();

  StringRef Path = "request.data";
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeCallbackStreamOutputBackend(
          [&](StringRef OutputPath,
              OutputConfig) -> std::unique_ptr<raw_pwrite_stream> {
            if (Path == OutputPath)
              return std::move(OS);
            return nullptr;
          }),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS)));

  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);
  EXPECT_NE(OriginalOS, O->getOS());

  // Write some data.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));
  EXPECT_EQ("some data", Output);

  // Lookup the file in FS. Ensure no errors when it creates a null-terminated
  // reference to the buffer.
  std::unique_ptr<MemoryBuffer> Buffer = openBufferForRead(*FS, Path);
  EXPECT_EQ(Path, Buffer->getBufferIdentifier());
  EXPECT_EQ("some data", Buffer->getBuffer());
}

TEST_F(OutputManagerTest, MirrorCallbackStreamInMemoryIgnored) {
  StringRef Path = "request.data";
  auto FS = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeCallbackStreamOutputBackend(
          [&](StringRef OutputPath, OutputConfig)
              -> std::unique_ptr<raw_pwrite_stream> { return nullptr; }),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS)));

  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);

  // Write some data.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));

  // Lookup the file in FS. It should be there even thought the callback didn't
  // capture a stream.
  std::unique_ptr<MemoryBuffer> Buffer = openBufferForRead(*FS, Path);
  EXPECT_EQ(Path, Buffer->getBufferIdentifier());
  EXPECT_EQ("some data", Buffer->getBuffer());
}

TEST_F(OutputManagerTest, MirrorInMemoryInMemory) {
  auto FS1 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  auto FS2 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS1),
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS2)));

  // Create the output.
  StringRef Path = "//root/in/memory.data";
  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));

  // Lookup the file.
  ASSERT_TRUE(FS1->exists(Path));
  ASSERT_TRUE(FS2->exists(Path));
  std::unique_ptr<MemoryBuffer> Buffer1 = openBufferForRead(*FS1, Path);
  std::unique_ptr<MemoryBuffer> Buffer2 = openBufferForRead(*FS2, Path);
  ASSERT_TRUE(Buffer1);
  ASSERT_TRUE(Buffer2);
  EXPECT_EQ(Path, Buffer1->getBufferIdentifier());
  EXPECT_EQ(Path, Buffer2->getBufferIdentifier());
  EXPECT_EQ("some data", Buffer1->getBuffer());
  EXPECT_EQ("some data", Buffer2->getBuffer());

  // Should be a reference to the same memory.
  EXPECT_EQ(Buffer1->getBufferStart(), Buffer2->getBufferStart());
}

TEST_F(OutputManagerTest, MirrorInMemoryInMemoryOwnBufferForMirror) {
  auto FS1 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  auto FS2 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  auto Backend2 = makeIntrusiveRefCnt<InMemoryOutputBackend>(FS2);
  Backend2->setShouldStoredBufferBeOwned(true);
  OutputManager M(makeMirroringOutputBackend(
      makeIntrusiveRefCnt<InMemoryOutputBackend>(FS1), std::move(Backend2)));

  // Create the output.
  StringRef Path = "//root/in/memory.data";
  auto O = expectedToPointer(M.createOutput(Path));
  ASSERT_TRUE(O);

  // Write some data into it, flush, and close.
  *O->takeOS() << "some data";
  EXPECT_FALSE(errorToBool(O->close()));

  // Lookup the file.
  ASSERT_TRUE(FS1->exists(Path));
  ASSERT_TRUE(FS2->exists(Path));
  std::unique_ptr<MemoryBuffer> Buffer1 = openBufferForRead(*FS1, Path);
  std::unique_ptr<MemoryBuffer> Buffer2 = openBufferForRead(*FS2, Path);
  ASSERT_TRUE(Buffer1);
  ASSERT_TRUE(Buffer2);
  EXPECT_EQ(Path, Buffer1->getBufferIdentifier());
  EXPECT_EQ(Path, Buffer2->getBufferIdentifier());
  EXPECT_EQ("some data", Buffer1->getBuffer());
  EXPECT_EQ("some data", Buffer2->getBuffer());

  // Should be a copy.
  EXPECT_NE(Buffer1->getBufferStart(), Buffer2->getBufferStart());
}

struct ScopedOutputManagerBackendTest : public OutputManagerTest {};

TEST_F(ScopedOutputManagerBackendTest, WritesToCorrectBackend) {
  auto FS1 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  auto FS2 = makeIntrusiveRefCnt<InMemoryFileSystem>();
  auto Backend1 = makeIntrusiveRefCnt<InMemoryOutputBackend>(FS1);
  auto Backend2 = makeIntrusiveRefCnt<InMemoryOutputBackend>(FS2);

  // Write five files: one while both scopes are in place, before and after
  // each nested scope.
  StringRef BeforeOuterPath = "//root/in/memory/before-outer.data";
  StringRef BeforeInnerPath = "//root/in/memory/before-inner.data";
  StringRef MiddlePath = "//root/in/memory/middle.data";
  StringRef AfterInnerPath = "//root/in/memory/after-inner.data";
  StringRef AfterOuterPath = "//root/in/memory/after-outer.data";
  OutputManager M(Backend1);
  EXPECT_EQ(Backend1.get(), &M.getBackend());
  {
    std::unique_ptr<Output> BeforeOuter, BeforeInner, Middle, AfterInner,
        AfterOuter;
    BeforeOuter = expectedToPointer(M.createOutput(BeforeOuterPath));
    ASSERT_TRUE(BeforeOuter);
    {
      ScopedOutputManagerBackend Outer(M, Backend2);
      EXPECT_EQ(Backend2.get(), &M.getBackend());
      BeforeInner = expectedToPointer(M.createOutput(BeforeInnerPath));
      ASSERT_TRUE(BeforeInner);
      {
        ScopedOutputManagerBackend Inner(M, Backend1);
        EXPECT_EQ(Backend1.get(), &M.getBackend());
        Middle = expectedToPointer(M.createOutput(MiddlePath));
        ASSERT_TRUE(Middle);
      }
      EXPECT_EQ(Backend2.get(), &M.getBackend());
      AfterInner = expectedToPointer(M.createOutput(AfterInnerPath));
      ASSERT_TRUE(AfterInner);
    }
    EXPECT_EQ(Backend1.get(), &M.getBackend());
    AfterOuter = expectedToPointer(M.createOutput(AfterOuterPath));
    ASSERT_TRUE(AfterOuter);

    EXPECT_FALSE(errorToBool(AfterInner->close()));
    EXPECT_FALSE(errorToBool(Middle->close()));
    EXPECT_FALSE(errorToBool(BeforeInner->close()));
    EXPECT_FALSE(errorToBool(AfterOuter->close()));
    EXPECT_FALSE(errorToBool(BeforeOuter->close()));
  }

  // Lookup the files. Middle and outer should be in FS1, while inner should be
  // in FS2.
  EXPECT_TRUE(FS1->exists(BeforeOuterPath));
  EXPECT_TRUE(FS1->exists(AfterOuterPath));
  EXPECT_TRUE(FS1->exists(MiddlePath));
  EXPECT_TRUE(!FS1->exists(BeforeInnerPath));
  EXPECT_TRUE(!FS1->exists(AfterInnerPath));
  EXPECT_TRUE(!FS2->exists(BeforeOuterPath));
  EXPECT_TRUE(!FS2->exists(AfterOuterPath));
  EXPECT_TRUE(!FS2->exists(MiddlePath));
  EXPECT_TRUE(FS2->exists(BeforeInnerPath));
  EXPECT_TRUE(FS2->exists(AfterInnerPath));
}

} // anonymous namespace
