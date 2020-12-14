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

#include "clang/Basic/OutputManager.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace llvm;

void OnDiskOutputRenameTempError::anchor() {}
void OutputAlreadyRequestedError::anchor() {}

char OnDiskOutputRenameTempError::ID = 0;
char OutputAlreadyRequestedError::ID = 0;

Error OutputManager::OnDiskOutput::initializeFD(StringRef OutputPath,
                                                OutputConfig Config) {
  // Disable temporary file for stdout (and return early since we won't use a
  // file descriptor directly).
  if (OutputPath == "-") {
    UseWriteThroughBuffer = false;
    return Error::success();
  }

  // Initialize UseWriteThroughBuffer.
  UseWriteThroughBuffer =
      CanWriteThroughBufferBeReused
          ? Config.test(OnDiskOutputConfig::UseWriteThroughWhenReused)
          : Config.test(OnDiskOutputConfig::UseWriteThroughWhenAlone);

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
    if (!tryToCreateTemporary(OutputPath, Config))
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

Error OutputManager::OnDiskOutput::initializeFile(StringRef OutputPath,
                                                  OutputConfig Config) {
  if (Error E = initializeFD(OutputPath, Config))
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

std::unique_ptr<llvm::raw_pwrite_stream>
OutputManager::OnDiskOutput::takeStream(OutputConfig Config) {
  assert(!Content &&
         "Need to write to stream in closeFile, can't give it away");
  assert(OS && "Expected file to be initialized");

  // Check whether we can get away with returning the stream directly.
  if (OS->supportsSeeking() || Config.test(OnDiskOutputConfig::OpenFlagText))
    return std::move(OS);

  // Wrap the raw_fd_ostream with a buffer if necessary.
  return std::make_unique<buffer_unique_ostream>(std::move(OS));
}

std::unique_ptr<llvm::raw_pwrite_stream>
OutputManager::RequestedOutput::takeStream() {
  assert(!Content && "Need to write to the stream later!");

  if (RequestedStream)
    return std::move(RequestedStream);

  // Ignore the output.
  return std::make_unique<raw_null_ostream>();
}

llvm::StringMapEntry<OutputManager::Output> &
OutputManager::getOrCreateNullHandle() {
  if (!Null) {
    NullMap.emplace();
    Null = &*NullMap->insert(std::make_pair("<null>", Output(OutputConfig())))
                 .first;
  }
  return *Null;
}

Expected<OutputManager::CreateOutputResult>
OutputManager::createOutput(StringRef OutputPath,
                            PartialOutputConfig Overrides) {
  // Create an active output.
  Expected<StringMap<Output>::iterator> OutputIt =
      createActiveOutput(OutputPath, Overrides);
  if (!OutputIt)
    return OutputIt.takeError();

  // If OutputPath wasn't added to ActiveOutputs but there's no error, then
  // this output is ignored entirely.
  if (*OutputIt == ActiveOutputs.end())
    return CreateOutputResult(getOrCreateNullHandle(),
                              std::make_unique<raw_null_ostream>());

  Output &O = (*OutputIt)->second;
  assert(!O.Content && "Already has content?");
  assert(!O.InMemory && "Already initialized in-memory?");
  assert(!O.OnDisk && "Already initialized on-disk?");

  // Construct output configurations and figure out if we need a content
  // buffer.
  bool NeedsContentBuffer = false;
  if (O.Config.test(OnDiskOutputConfig::Enabled)) {
    O.OnDisk.emplace();
    O.OnDisk->CanWriteThroughBufferBeReused =
        O.Config.test(InMemoryOutputConfig::Enabled) &&
        O.Config.test(InMemoryOutputConfig::ReuseWriteThroughBuffer);

    // Initialize the file for on-disk outputs to detect any errors up front,
    // even if we won't write to the file until later.
    if (Error E = O.OnDisk->initializeFile(OutputPath, O.Config))
      return std::move(E);

    NeedsContentBuffer |= O.OnDisk->useWriteThroughBuffer();
  }

  if (O.Config.test(InMemoryOutputConfig::Enabled)) {
    O.InMemory.emplace();
    NeedsContentBuffer = true;
  }

  if (O.Request && O.Request->needsContent())
    if (O.OnDisk || O.InMemory)
      NeedsContentBuffer = true;

  // Construct the content buffer.
  if (NeedsContentBuffer) {
    O.Content.emplace();
    if (O.InMemory)
      O.InMemory->Content = &*O.Content;
    if (O.OnDisk)
      O.OnDisk->Content = &*O.Content;
    if (O.Request && O.Request->needsContent())
      O.Request->Content = &*O.Content;
  }

  // Figure out the stream to use.
  std::unique_ptr<raw_pwrite_stream> OS;
  if (O.Content) {
    OS = std::make_unique<raw_svector_ostream>(*O.Content);
  } else if (O.OnDisk) {
    OS = O.OnDisk->takeStream(O.Config);
  } else {
    assert(O.Request && "Expected no-request-no-content to be handled already");
    OS = O.Request->takeStream();
  }
  assert(OS && "Expected stream to be initialized");

  return CreateOutputResult(**OutputIt, std::move(OS));
}

Expected<StringMap<OutputManager::Output>::iterator>
OutputManager::createActiveOutput(StringRef OutputPath,
                                  PartialOutputConfig Overrides) {
  StringMap<Output>::iterator RequestIt = Requests.find(OutputPath);
  Optional<Output> O;
  if (RequestIt == Requests.end()) {
    O.emplace(Defaults);
  } else {
    // There has been a request for this output, although it could just be a
    // configuration change.
    O = std::move(RequestIt->second);
    Requests.erase(RequestIt);
  }

  // Apply overrides.
  O->Config.applyOverrides(Overrides);

  // Check if outputs are being ignored / dropped by default.
  if (!O->Request && !O->Config.test(InMemoryOutputConfig::Enabled) &&
      !O->Config.test(OnDiskOutputConfig::Enabled))
    return ActiveOutputs.end();

  // If it's not ignored, try to insert it, and error on failure.
  auto OutputInsertion =
      ActiveOutputs.insert(std::make_pair(OutputPath, std::move(*O)));
  if (!OutputInsertion.second)
    return createFileError(OutputPath,
                           errorCodeToError(std::make_error_code(
                               std::errc::no_such_file_or_directory)));

  return OutputInsertion.first;
}

std::error_code
OutputManager::OnDiskOutput::tryToCreateTemporary(StringRef OutputPath,
                                                  OutputConfig Config) {
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

Error OutputManager::OnDiskOutput::closeFile(
    StringRef OutputPath, std::unique_ptr<MemoryBuffer> &Buffer) {
  if (useWriteThroughBuffer()) {
    assert(FD && "Write-through buffer needs a file descriptor");
    assert(!OS && "Expected no stream when using write-through buffer");
    assert(Content);
    if (auto EC = sys::fs::resize_file(*FD, Content->size()))
      return errorCodeToError(EC);
    ErrorOr<std::unique_ptr<WriteThroughMemoryBuffer>> WriteThroughBuffer =
        WriteThroughMemoryBuffer::getFile(*FD, OutputPath, Content->size());
    if (!WriteThroughBuffer)
      return errorCodeToError(WriteThroughBuffer.getError());

    std::memcpy((*WriteThroughBuffer)->getBufferStart(), Content->begin(),
                Content->size());
    Buffer = std::move(*WriteThroughBuffer);
  } else if (OS) {
    // Write out the content and close the stream.
    assert(Content && "Need to write content to a stream");
    *OS << *Content;
    OS = nullptr;
  } else {
    assert(!Content && "Content in memory with no way to write it to disk?");
  }

  if (!TempPath)
    return Error::success();

  // Move temporary to the final output path.
  std::error_code EC = sys::fs::rename(*TempPath, OutputPath);
  if (!EC)
    return Error::success();
  (void)sys::fs::remove(*TempPath);
  return createOnDiskOutputRenameTempError(*TempPath, OutputPath, EC);
}

void OutputManager::OnDiskOutput::eraseFile(StringRef OutputPath) {
  // If there's no FD we haven't created a file.
  if (!FD)
    return;

  // If there's a temporary file, that's the one to delete.
  if (TempPath)
    sys::fs::remove(*TempPath);
  else
    sys::fs::remove(OutputPath);
}

void OutputManager::RequestedOutput::copyContent() {
  if (!RequestedStream)
    return;

  assert(Content);
  *RequestedStream << *Content;
  RequestedStream = nullptr;
}

void OutputManager::InMemoryOutput::storeContent(
    StringRef OutputPath, OutputConfig Config,
    vfs::InMemoryFileSystem &InMemoryFS,
    std::unique_ptr<llvm::MemoryBuffer> Buffer) {
  static unsigned PageSize = sys::Process::getPageSizeEstimate();
  constexpr unsigned MinimumSizeToReuseWriteThroughBuffer = 4 * 4096;

  // Decide whether to reuse the provided write-through buffer. Even if
  // configured to use it, skip it when it's too small (fragmenting memory) or
  // page-aligned (missing a null-terminator).
  if (Buffer) {
    const size_t BufferSize = Buffer->getBufferSize();
    assert(BufferSize == Content->size() &&
           "Expected incoming buffer to be equivalent to content");
    if (!Config.test(InMemoryOutputConfig::ReuseWriteThroughBuffer) ||
        BufferSize < MinimumSizeToReuseWriteThroughBuffer ||
        (BufferSize & (PageSize - 1)) == 0)
      Buffer = nullptr;
  }

  // Construct a buffer from Content if necessary.
  if (!Buffer) {
    // Ensure this is null-terminated.
    Content->push_back(0);
    Content->pop_back();
    Buffer = std::make_unique<SmallVectorMemoryBuffer>(std::move(*Content));
  }

  InMemoryFS.addFile(OutputPath, 0, std::move(Buffer));
}

llvm::Error
OutputManager::requestOutput(StringRef OutputPath,
                             std::unique_ptr<llvm::raw_pwrite_stream> OS,
                             llvm::unique_function<void(StringRef)> OnClose,
                             PartialOutputConfig Overrides) {
  // Try to queue up a request.
  auto RequestInsertion =
      Requests.insert(std::make_pair(OutputPath, Output(Defaults)));
  if (!RequestInsertion.second)
    return createOutputAlreadyRequestedError(OutputPath);

  /// Apply overrides.
  Output &O = RequestInsertion.first->second;
  O.Config.applyOverrides(Overrides);

  // Set up the requested callback and stream.
  if (OS || OnClose)
    O.Request.emplace(std::move(OnClose), std::move(OS));
  return Error::success();
}

void OutputManager::RequestedOutput::reportOnClose(StringRef OutputPath) {
  if (OnClose)
    OnClose(OutputPath);
}

bool OutputManager::isNullHandle(OutputHandle Handle) const {
  return &Handle.O.get() == Null;
}

Error OutputManager::closeOutput(OutputHandle Handle) {
  // Check for a dummy handle to null.
  if (isNullHandle(Handle))
    return Error::success();

  std::unique_ptr<StringMapEntry<Output>> Entry(&Handle.O.get());
  ActiveOutputs.remove(Entry.get());

  StringRef OutputPath = Entry->first();
  Output &O = Entry->second;

  if (O.Request)
    O.Request->copyContent();

  std::unique_ptr<MemoryBuffer> Buffer;
  if (O.OnDisk)
    if (Error E = O.OnDisk->closeFile(OutputPath, Buffer))
      return E;

  // Note: Don't read O.Content after this call in case it's moved away.
  if (O.InMemory)
    O.InMemory->storeContent(OutputPath, O.Config, *InMemoryFS,
                             std::move(Buffer));

  // Run the on-close call-back *last* in case it triggers a check of on-disk
  // or in-memory content.
  if (O.Request)
    O.Request->reportOnClose(OutputPath);

  return Error::success();
}

void OutputManager::eraseOutput(OutputHandle Handle) {
  // Check for a dummy handle to null.
  if (isNullHandle(Handle))
    return;

  std::unique_ptr<StringMapEntry<Output>> Entry(&Handle.O.get());
  ActiveOutputs.remove(Entry.get());

  StringRef OutputPath = Entry->first();
  Output &O = Entry->second;

  if (O.OnDisk)
    O.OnDisk->eraseFile(OutputPath);
}

Expected<std::unique_ptr<raw_pwrite_stream>>
OutputContext::createOutput(StringRef OutputPath,
                            PartialOutputConfig Overrides) {
  Optional<SmallString<128>> AbsPath;
  if (!sys::path::is_absolute(OutputPath) && !WorkingDir.empty()) {
    AbsPath.emplace(WorkingDir);
    sys::path::append(*AbsPath, OutputPath);
    OutputPath = *AbsPath;
  }
  Expected<OutputManager::CreateOutputResult> Output =
      getManager().createOutput(OutputPath,
                                Overrides.applyDefaults(DefaultOverrides));
  if (!Output)
    return Output.takeError();

  Outputs.push_back(Output->Handle);
  return std::move(Output->OS);
}

Error OutputContext::closeAllOutputs(bool ShouldErase) {
  if (ShouldErase) {
    eraseAllOutputs();
    return Error::success();
  }

  Error E = Error::success();
  for (auto Handle : Outputs)
    E = joinErrors(std::move(E), getManager().closeOutput(Handle));

  Outputs.clear();
  return E;
}

void OutputContext::eraseAllOutputs() {
  for (auto Handle : Outputs)
    getManager().eraseOutput(Handle);

  Outputs.clear();
}
