//===- OutputManager.cpp - Manage compiler outputs ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements the OutputManager interface.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/OutputManager.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::vfs;

void OutputError::anchor() {}
char OutputError::ID = 0;

void CannotOverwriteExistingOutputError::anchor() {}
char CannotOverwriteExistingOutputError::ID = 0;

void OnDiskOutputRenameTempError::anchor() {}
char OnDiskOutputRenameTempError::ID = 0;

Output::~Output() = default;

void Output::initializeOS() {
  if (!Dest) {
    OS = std::make_unique<raw_null_ostream>();
    return;
  }
  OS = Dest->takeStream();
  if (!OS) {
    // This destination needs a content buffer.
    Bytes.emplace();
    OS = std::make_unique<raw_svector_ostream>(*Bytes);
  }
}

Error Output::close(bool ShouldErase) {
  assert(isOpen() && "Output already closed or erased");

  // Forward to Output::erase() if necessary.
  if (ShouldErase) {
    erase();
    return Error::success();
  }

  // Mark as closed and destruct the stream if we still own it.
  IsOpen = false;
  OS = nullptr;

  if (!Dest)
    return Error::success();

  // Clear the destination on exit.
  std::unique_ptr<OutputDestination> MovedDest = std::move(Dest);

  if (!Bytes)
    return MovedDest->storeStreamedContent();
  return MovedDest->storeContent(
      OutputDestination::ContentBuffer(std::move(*Bytes), Path));
}

void Output::erase() {
  assert(isOpen() && "Output already closed or erased");
  IsOpen = false;

  // Close the stream first in case its destructor references Bytes or Dest.
  OS = nullptr;

  // Erase any temporary content from the destination.
  Dest = nullptr;

  // Free the content buffer.
  Bytes = None;
}

void OutputDestination::anchor() {}
void OutputBackend::anchor() {}

void OutputDestination::ContentBuffer::finishConstructingFromVector(
    StringRef Identifier) {
  // If Vector is empty, we can't guarantee SmallVectorMemoryBuffer will keep
  // pointer identity. But since there's no content, it's all moot; just
  // construct a reference.
  if (Vector->empty()) {
    Vector = None;
    Buffer.emplace("", Identifier);
    return;
  }

  // Ensure null-termination.
  Vector->push_back(0);
  Vector->pop_back();
  Buffer.emplace(StringRef(Vector->begin(), Vector->size()), Identifier);
}

std::unique_ptr<MemoryBuffer>
OutputDestination::ContentBuffer::takeOwnedBufferOrNull() {
  assert(Buffer && "Expected initialization?");
  if (OwnedBuffer)
    return std::move(OwnedBuffer);

  if (!Vector)
    return nullptr;

  bool IsEmpty = Vector->empty();
  (void)IsEmpty;
  auto VectorBuffer = std::make_unique<SmallVectorMemoryBuffer>(
      std::move(*Vector), Buffer->getBufferIdentifier());
  Vector = None;
  assert(Buffer->getBufferStart() == VectorBuffer->getBufferStart());
  assert(Buffer->getBufferEnd() == VectorBuffer->getBufferEnd());
  return VectorBuffer;
}

std::unique_ptr<MemoryBuffer> OutputDestination::ContentBuffer::takeBuffer() {
  assert(Buffer && "Expected initialization?");
  if (std::unique_ptr<MemoryBuffer> B = takeOwnedBufferOrNull())
    return B;
  return MemoryBuffer::getMemBuffer(*Buffer);
}

std::unique_ptr<MemoryBuffer>
OutputDestination::ContentBuffer::takeOwnedBufferOrCopy() {
  assert(Buffer && "Expected initialization?");
  if (std::unique_ptr<MemoryBuffer> B = takeOwnedBufferOrNull())
    return B;
  return MemoryBuffer::getMemBufferCopy(Buffer->getBuffer(),
                                        Buffer->getBufferIdentifier());
}

OutputManager::OutputManager()
    : OutputManager(std::make_unique<OnDiskOutputBackend>()) {}

Expected<std::unique_ptr<Output>>
OutputManager::createOutput(StringRef OutputPath,
                            PartialOutputConfig Overrides) {
  OutputConfig Config = Defaults;
  Config.applyOverrides(Overrides);

  Expected<std::unique_ptr<OutputDestination>> ExpectedDest =
      getBackend().createDestination(OutputPath, Config, nullptr);
  if (!ExpectedDest)
    return ExpectedDest.takeError();

  return std::make_unique<Output>(OutputPath, Config, std::move(*ExpectedDest));
}

namespace {
class NullOutputBackend : public OutputBackend {
public:
  Expected<std::unique_ptr<OutputDestination>>
  createDestination(StringRef, OutputConfig,
                    std::unique_ptr<OutputDestination> NextDest) final {
    return std::move(NextDest);
  }
};
} // anonymous namespace

IntrusiveRefCntPtr<OutputBackend> llvm::vfs::makeNullOutputBackend() {
  return std::make_unique<NullOutputBackend>();
}

namespace {
class OnDiskOutputDestination : public OutputDestination {
public:
  /// Open a file on-disk for writing to \a OutputPath.
  ///
  /// This calls \a initializeFD() to initialize \a FD (see that
  /// documentation for more detail) and \a UseWriteThroughBuffer. Unless \a
  /// UseWriteThroughBuffer was determined to be \c true, this function will
  /// then initialize \a OS with a valid stream.
  Error initializeFile();

  /// Erases the file if it hasn't be closed. If \a TempFile is set, erases it;
  /// otherwise, erases \a OutputPath.
  ~OnDiskOutputDestination() override;

  /// Take an open output stream.
  std::unique_ptr<raw_pwrite_stream> takeStreamImpl() final;

  Error storeStreamedContentImpl() final {
    return closeFile(OutputPath, nullptr);
  }

  Error storeContentImpl(ContentBuffer &Content) final {
    return closeFile(Content.getPath(), &Content);
  }

private:
  /// Returns true if the final write uses a \a
  /// WriteThroughMemoryBuffer. This is only valid to call after \a
  /// initializeFile().
  bool useWriteThroughBuffer() const {
    assert(UseWriteThroughBuffer != None);
    return *UseWriteThroughBuffer;
  }

  using CheckingContentConsumerType =
      function_ref<Error(Optional<ContentBuffer>)>;

  /// Close a file after successfully collecting the output.
  ///
  /// The output content must have already been written somewhere, but
  /// exactly where depends on the configuration.
  ///
  /// - If \a WriteThroughMemoryBuffer is \c true, the output will be in \a
  ///   Content and \a FD has an open file descriptor. In that case, the file
  ///   will be completed by opening \a WriteThroughMemoryBuffer and
  ///   calling \a std::memcpy().
  /// - Else if the output is in \a Content, it will be written to \a OS,
  ///   which should be an already-open file stream, and the content will be
  ///   written there and the stream destroyed to flush it.
  /// - Else the content should already be in the file.
  ///
  /// Once the content is on-disk, if \a TempPath is set, this calls \a
  /// sys::rename() to move the file to \a OutputPath.
  ///
  /// Returns a file-backed MemoryBuffer when the file was written using a
  /// write-through memory buffer, unless it won't be null-terminated or it's
  /// too small. Otherwise, when there's no error, returns \c nullptr.
  Error closeFile(StringRef OutputPath, ContentBuffer *MaybeContent);

  /// Attempt to open a temporary file for \a OutputPath.
  ///
  /// This tries to open a uniquely-named temporary file for \a OutputPath,
  /// possibly also creating any missing directories if \a
  /// OnDiskOutputConfig::UseTemporaryCreateMissingDirectories is set in \a
  /// Config.
  ///
  /// \post FD and \a TempPath are initialized if this is successful.
  std::error_code tryToCreateTemporary();

  /// Open a file on-disk for writing to OutputPath.
  ///
  /// This opens a file for writing and assigns the file descriptor to \c FD.
  /// Exactly how the file is opened depends on the \a OnDiskOutputConfig
  /// settings in \p Config and (if the file exists) the type of file.
  ///
  /// - If \p outputPath is \c "-" (indicating stdin), this function returns
  ///   \a Error::success() but has no effect.
  /// - \a OnDiskOutputConfig::UseTemporary requests that a temporary file is
  ///   opened first, to be renamed to \p OutputPath when \a closeFile() is
  ///   called. This is disabled if \p OutputPath exists and \a
  ///   sys::fs::is_regular_file() returns \c false (such as a named
  ///   pipe). If \a tryToCreateTemporary() fails, this falls back to
  ///   no-temp-file mode. (Even if using a temporary file, \p OutputPath is
  ///   checked for write permission.)
  /// - \a OnDiskOutputConfig::RemoveFileOnSignal installs a signal handler
  ///   to remove the opened file.
  ///
  /// This function also initializes \a UseWriteThroughBuffer, depending on \a
  /// OnDiskOutputConfig::UseWriteThroughBuffer and whether the file type
  /// supports them.
  ///
  /// \post UseWriteThroughBuffer is set.
  /// \post FD is set unless \a OutputPath is \c "-" or on error.
  /// \post TempPath is set if a temporary file was opened successfully.
  Error initializeFD();

public:
  OnDiskOutputDestination(StringRef OutputPath, OutputConfig Config,
                          std::unique_ptr<OutputDestination> Next)
      : OutputDestination(std::move(Next)), OutputPath(OutputPath.str()),
        Config(Config) {}

private:
  std::string OutputPath;
  OutputConfig Config;
  Optional<bool> UseWriteThroughBuffer;
  bool IsOpen = true;
  std::unique_ptr<raw_fd_ostream> OS;
  Optional<std::string> TempPath;
  Optional<int> FD;
};
} // anonymous namespace

void OnDiskOutputBackend::anchor() {}

std::error_code OnDiskOutputDestination::tryToCreateTemporary() {
  // Create a temporary file.
  // Insert -%%%%%%%% before the extension (if any), and because some tools
  // (noticeable, clang's own GlobalModuleIndex.cpp) glob for build
  // artifacts, also append .tmp.
  StringRef OutputExtension = sys::path::extension(OutputPath);
  SmallString<128> TempPath =
      StringRef(OutputPath).drop_back(OutputExtension.size());
  TempPath += "-%%%%%%%%";
  TempPath += OutputExtension;
  TempPath += ".tmp";

  auto tryToCreateImpl = [&]() {
    int NewFD;
    if (std::error_code EC =
            sys::fs::createUniqueFile(TempPath, NewFD, TempPath))
      return EC;
    if (Config.test(OnDiskOutputConfig::RemoveFileOnSignal))
      sys::RemoveFileOnSignal(TempPath);
    this->TempPath = TempPath.str().str();
    FD = NewFD;
    return std::error_code();
  };

  // Try to create the temporary.
  std::error_code EC = tryToCreateImpl();
  if (!EC)
    return std::error_code();

  if (EC != std::errc::no_such_file_or_directory ||
      !Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories))
    return EC;

  // Create parent directories and try again.
  StringRef ParentPath = sys::path::parent_path(OutputPath);
  if ((EC = sys::fs::create_directories(ParentPath)))
    return EC;
  return tryToCreateImpl();
}

Error OnDiskOutputDestination::initializeFD() {
  // Disable temporary file for stdout (and return early since we won't use a
  // file descriptor directly).
  if (OutputPath == "-") {
    UseWriteThroughBuffer = false;
    return Error::success();
  }

  // Initialize UseWriteThroughBuffer.
  UseWriteThroughBuffer =
      Config.test(OnDiskOutputConfig::UseWriteThroughBuffer);

  // Function to check and update whether to use a write-through buffer.
  bool CheckedStatusForWriteThrough = false;
  auto checkStatusForWriteThrough = [&](const sys::fs::file_status &Status) {
    if (CheckedStatusForWriteThrough)
      return;
    sys::fs::file_type Type = Status.type();
    if (Type != sys::fs::file_type::regular_file &&
        Type != sys::fs::file_type::block_file)
      UseWriteThroughBuffer = false;
    CheckedStatusForWriteThrough = true;
  };

  // Disable temporary file for other non-regular files, and if we get a status
  // object, also check if we can write and disable write-through buffers if
  // appropriate.
  if (Config.test(OnDiskOutputConfig::UseTemporary)) {
    sys::fs::file_status Status;
    sys::fs::status(OutputPath, Status);
    if (sys::fs::exists(Status)) {
      if (!sys::fs::is_regular_file(Status))
        Config.reset(OnDiskOutputConfig::UseTemporary);

      // Fail now if we can't write to the final destination.
      if (!sys::fs::can_write(OutputPath))
        return errorCodeToError(
            std::make_error_code(std::errc::operation_not_permitted));

      checkStatusForWriteThrough(Status);
    }
  }

  // If (still) using a temporary file, try to create it (and return success if
  // that works).
  if (Config.test(OnDiskOutputConfig::UseTemporary))
    if (!tryToCreateTemporary())
      return Error::success();

  // Not using a temporary file. Open the final output file.
  int NewFD;
  if (auto EC = sys::fs::openFileForWrite(
          OutputPath, NewFD, sys::fs::CD_CreateAlways,
          Config.test(OnDiskOutputConfig::OpenFlagText) ? sys::fs::OF_Text
                                                        : sys::fs::OF_None))
    return errorCodeToError(EC);
  FD = NewFD;

  // Check the status with the open FD to see whether a write-through buffer
  // makes sense.
  if (*UseWriteThroughBuffer && !CheckedStatusForWriteThrough) {
    sys::fs::file_status Status;
    sys::fs::status(NewFD, Status);
    checkStatusForWriteThrough(Status);
  }

  if (Config.test(OnDiskOutputConfig::RemoveFileOnSignal))
    sys::RemoveFileOnSignal(OutputPath);
  return Error::success();
}

Error OnDiskOutputDestination::initializeFile() {
  if (Error E = initializeFD())
    return E;

  assert(FD || OutputPath == "-");
  if (useWriteThroughBuffer())
    return Error::success();

  // Open the raw_fd_ostream right away to free the file descriptor.
  std::error_code EC;
  OS = OutputPath == "-"
           ? std::make_unique<raw_fd_ostream>(OutputPath, EC)
           : std::make_unique<raw_fd_ostream>(*FD, /*shouldClose=*/true);
  assert(!EC && "Unexpected error opening stdin");

  return Error::success();
}

std::unique_ptr<raw_pwrite_stream> OnDiskOutputDestination::takeStreamImpl() {
  if (useWriteThroughBuffer())
    return nullptr;

  assert(OS && "Expected file to be initialized");

  // Check whether we can get away with returning the stream directly.
  if (OS->supportsSeeking() ||
      !Config.test(ClientIntentOutputConfig::NeedsSeeking))
    return std::move(OS);

  // Wrap the raw_fd_ostream with a buffer if necessary.
  return std::make_unique<buffer_unique_ostream>(std::move(OS));
}

Error OnDiskOutputDestination::closeFile(StringRef OutputPath,
                                         ContentBuffer *MaybeContent) {
  assert(IsOpen && "can't close a file twice");
  IsOpen = false;
  assert((!MaybeContent || MaybeContent->getPath() == OutputPath) &&
         "Expected buffer identifier to match output path");

  Optional<StringRef> BufferedContent;
  if (MaybeContent)
    BufferedContent = MaybeContent->getBytes();
  if (useWriteThroughBuffer()) {
    assert(FD && "Write-through buffer needs a file descriptor");
    assert(!OS && "Expected no stream when using write-through buffer");
    assert(BufferedContent && "Expected buffered content");
    if (auto EC = sys::fs::resize_file(*FD, BufferedContent->size()))
      return errorCodeToError(EC);
    ErrorOr<std::unique_ptr<WriteThroughMemoryBuffer>> WriteThroughBuffer =
        WriteThroughMemoryBuffer::getOpenFile(*FD, OutputPath,
                                              BufferedContent->size());
    if (!WriteThroughBuffer)
      return errorCodeToError(WriteThroughBuffer.getError());

    std::memcpy((*WriteThroughBuffer)->getBufferStart(),
                BufferedContent->begin(), BufferedContent->size());

    // Decide whether to return the file-backed buffer.  Skip it when it's too
    // small (fragmenting memory) or page-aligned (missing a null-terminator).
    static unsigned PageSize = sys::Process::getPageSizeEstimate();
    if (MaybeContent &&
        BufferedContent->size() >=
            OnDiskOutputBackend::MinimumSizeToReturnWriteThroughBuffer &&
        (BufferedContent->size() & (PageSize - 1)) != 0) {
      assert((*WriteThroughBuffer)->getBufferStart()[BufferedContent->size()] ==
                 0 &&
             "Expected non-page-aligned mmap region to be null-terminated");
      *MaybeContent = ContentBuffer(std::move(*WriteThroughBuffer));
    }
  } else if (OS) {
    // Write out the content and close the stream.
    assert(BufferedContent && "Need to write content to a stream");
    OS->write(BufferedContent->begin(), BufferedContent->size());
    OS = nullptr;
  } else {
    assert(!BufferedContent &&
           "Content in memory with no way to write it to disk?");
  }

  // Return early if there's no temporary path.
  if (!TempPath)
    return Error::success();

  // Move temporary to the final output path.
  std::error_code EC = sys::fs::rename(*TempPath, OutputPath);
  if (!EC)
    return Error::success();
  (void)sys::fs::remove(*TempPath);
  return createOnDiskOutputRenameTempError(*TempPath, OutputPath, EC);
}

OnDiskOutputDestination::~OnDiskOutputDestination() {
  if (!IsOpen)
    return;

  // If there's no FD we haven't created a file.
  if (!FD)
    return;

  // If there's a temporary file, that's the one to delete.
  if (TempPath)
    sys::fs::remove(*TempPath);
  else
    sys::fs::remove(OutputPath);
}

Expected<std::unique_ptr<OutputDestination>>
OnDiskOutputBackend::createDestination(
    StringRef OutputPath, OutputConfig Config,
    std::unique_ptr<OutputDestination> NextDest) {
  auto Dest = std::make_unique<OnDiskOutputDestination>(OutputPath, Config,
                                                        std::move(NextDest));
  if (Error E = Dest->initializeFile())
    return std::move(E);
  return Dest;
}

namespace {
class InMemoryOutputDestination : public OutputDestination {
public:
  bool shouldStoredBufferBeOwned() const {
    return Backend->shouldStoredBufferBeOwned();
  }

  Error storeContentImpl(ContentBuffer &Content) final;

  InMemoryOutputDestination(IntrusiveRefCntPtr<InMemoryOutputBackend> Backend,
                            std::unique_ptr<OutputDestination> Next)
      : OutputDestination(std::move(Next)), Backend(std::move(Backend)) {}

protected:
  IntrusiveRefCntPtr<InMemoryOutputBackend> Backend;
};
} // anonymous namespace

void InMemoryOutputBackend::anchor() {}

Error InMemoryOutputDestination::storeContentImpl(ContentBuffer &Content) {
  assert(Content.getBytes().end()[0] == 0 && "Expected null-terminated buffer");
  InMemoryFileSystem &FS = Backend->getInMemoryFS();
  std::unique_ptr<MemoryBuffer> Buffer = shouldStoredBufferBeOwned()
                                             ? Content.takeOwnedBufferOrCopy()
                                             : Content.takeBuffer();
  StringRef OutputPath = Content.getPath();
  if (FS.exists(OutputPath) || !FS.addFile(OutputPath, 0, std::move(Buffer)))
    return createCannotOverwriteExistingOutputError(OutputPath);
  return Error::success();
}

Expected<std::unique_ptr<OutputDestination>>
InMemoryOutputBackend::createDestination(
    StringRef OutputPath, OutputConfig,
    std::unique_ptr<OutputDestination> NextDest) {
  if (InMemoryFS->exists(OutputPath))
    return createCannotOverwriteExistingOutputError(OutputPath);
  return std::make_unique<InMemoryOutputDestination>(this, std::move(NextDest));
}

namespace {
class MirroringOutputBackend : public OutputBackend {
public:
  Expected<std::unique_ptr<OutputDestination>>
  createDestination(StringRef OutputPath, OutputConfig Config,
                    std::unique_ptr<OutputDestination> NextDest) override;

  MirroringOutputBackend(IntrusiveRefCntPtr<OutputBackend> Backend1,
                         IntrusiveRefCntPtr<OutputBackend> Backend2)
      : Backend1(Backend1), Backend2(Backend2) {}

private:
  IntrusiveRefCntPtr<OutputBackend> Backend1;
  IntrusiveRefCntPtr<OutputBackend> Backend2;
};
} // anonymous namespace

IntrusiveRefCntPtr<OutputBackend> llvm::vfs::makeMirroringOutputBackend(
    IntrusiveRefCntPtr<OutputBackend> Backend1,
    IntrusiveRefCntPtr<OutputBackend> Backend2) {
  return std::make_unique<MirroringOutputBackend>(std::move(Backend1),
                                                  std::move(Backend2));
}

Expected<std::unique_ptr<OutputDestination>>
MirroringOutputBackend::createDestination(
    StringRef OutputPath, OutputConfig Config,
    std::unique_ptr<OutputDestination> NextDest) {
  // Visit in reverse order so that the first backend gets the content before
  // the second one.
  for (OutputBackend *B : {&*Backend2, &*Backend1}) {
    Expected<std::unique_ptr<OutputDestination>> Dest =
        B->createDestination(OutputPath, Config, std::move(NextDest));
    if (!Dest)
      return Dest.takeError();
    NextDest = std::move(*Dest);
  }

  return std::move(NextDest);
}

namespace {
class FilteringOutputBackend : public OutputBackend {
public:
  Expected<std::unique_ptr<OutputDestination>>
  createDestination(StringRef OutputPath, OutputConfig Config,
                    std::unique_ptr<OutputDestination> NextDest) override {
    if (!Filter(OutputPath, Config))
      return std::move(NextDest);
    return UnderlyingBackend->createDestination(OutputPath, Config,
                                                std::move(NextDest));
  }

  using FilterType = unique_function<bool(StringRef, OutputConfig)>;
  FilteringOutputBackend(IntrusiveRefCntPtr<OutputBackend> UnderlyingBackend,
                         FilterType Filter)
      : UnderlyingBackend(std::move(UnderlyingBackend)),
        Filter(std::move(Filter)) {}

private:
  IntrusiveRefCntPtr<OutputBackend> UnderlyingBackend;
  FilterType Filter;
};
} // anonymous namespace

IntrusiveRefCntPtr<OutputBackend> llvm::vfs::makeFilteringOutputBackend(
    IntrusiveRefCntPtr<OutputBackend> UnderlyingBackend,
    FilteringOutputBackend::FilterType Filter) {
  return std::make_unique<FilteringOutputBackend>(std::move(UnderlyingBackend),
                                                  std::move(Filter));
}

namespace {
class CallbackStreamOutputBackend : public OutputBackend {
public:
  Expected<std::unique_ptr<OutputDestination>>
  createDestination(StringRef OutputPath, OutputConfig Config,
                    std::unique_ptr<OutputDestination> NextDest) override;

  CallbackStreamOutputBackend(
      unique_function<
          Expected<std::unique_ptr<raw_pwrite_stream>>(StringRef, OutputConfig)>
          OnOpen,
      unique_function<Error(StringRef)> OnClose,
      unique_function<void(StringRef)> OnErase)
      : OnOpen(std::move(OnOpen)), OnClose(std::move(OnClose)),
        OnErase(std::move(OnErase)) {}

private:
  friend class CallbackStreamOutputDestination;
  unique_function<Expected<std::unique_ptr<raw_pwrite_stream>>(StringRef,
                                                               OutputConfig)>
      OnOpen;
  unique_function<Error(StringRef)> OnClose;
  unique_function<void(StringRef)> OnErase;
};

class CallbackStreamOutputDestination : public OutputDestination {
public:
  ~CallbackStreamOutputDestination() override {
    if (!IsOpen)
      return;
    Stream = nullptr;
    if (Backend->OnErase)
      Backend->OnErase(OutputPath);
  }

  std::unique_ptr<raw_pwrite_stream> takeStreamImpl() final {
    if (Stream)
      return std::move(Stream);
    return std::make_unique<raw_null_ostream>();
  }

  Error storeStreamedContentImpl() final {
    assert(IsOpen);
    IsOpen = false;
    assert(!Stream && "Expected stream to be taken already?");
    return Backend->OnClose ? Backend->OnClose(OutputPath) : Error::success();
  }

  Error storeContentImpl(ContentBuffer &Content) final {
    assert(IsOpen);
    IsOpen = false;
    assert(Stream && "Expected stream for writing content");
    Stream->write(Content.getBytes().begin(), Content.getBytes().size());
    Stream = nullptr;
    if (Backend->OnClose)
      if (Error E = Backend->OnClose(Content.getPath()))
        return E;
    return Error::success();
  }

  CallbackStreamOutputDestination(
      IntrusiveRefCntPtr<CallbackStreamOutputBackend> Backend,
      std::unique_ptr<raw_pwrite_stream> Stream, StringRef OutputPath,
      std::unique_ptr<OutputDestination> Next)
      : OutputDestination(std::move(Next)), OutputPath(OutputPath.str()),
        Backend(std::move(Backend)), Stream(std::move(Stream)) {}

private:
  std::string OutputPath;
  bool IsOpen = true;
  IntrusiveRefCntPtr<CallbackStreamOutputBackend> Backend;
  std::unique_ptr<raw_pwrite_stream> Stream;
};
} // anonymous namespace

IntrusiveRefCntPtr<OutputBackend> llvm::vfs::makeCallbackStreamOutputBackend(
    unique_function<Expected<std::unique_ptr<raw_pwrite_stream>>(StringRef,
                                                                 OutputConfig)>
        OnOpen,
    unique_function<Error(StringRef)> OnClose,
    unique_function<void(StringRef)> OnErase) {
  return std::make_unique<CallbackStreamOutputBackend>(
      std::move(OnOpen), std::move(OnClose), std::move(OnErase));
}

Expected<std::unique_ptr<OutputDestination>>
CallbackStreamOutputBackend::createDestination(
    StringRef OutputPath, OutputConfig Config,
    std::unique_ptr<OutputDestination> NextDest) {
  std::unique_ptr<raw_pwrite_stream> Stream;
  if (OnOpen) {
    auto ExpectedStream = OnOpen(OutputPath, Config);
    if (!ExpectedStream)
      return ExpectedStream.takeError();
    Stream = std::move(*ExpectedStream);
  }

  // Check if this is being ignored entirely.
  if (!Stream && !OnClose && !OnErase)
    return std::move(NextDest);

  return std::make_unique<CallbackStreamOutputDestination>(
      this, std::move(Stream), OutputPath, std::move(NextDest));
}
