//===- VirtualOutputBackends.cpp - Virtual output backends ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements vfs::OutputBackend.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/VirtualOutputBackends.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"

using namespace llvm;
using namespace llvm::vfs;

void OnDiskOutputBackend::anchor() {}

IntrusiveRefCntPtr<OutputBackend> vfs::makeNullOutputBackend() {
  struct NullOutputBackend : public OutputBackend {
    struct NullOutput final : public OutputFileImpl {
      Error keep() override { return Error::success(); }
      Error discard() override { return Error::success(); }
      raw_pwrite_stream &getOS() override { return OS; }
      raw_null_ostream OS;
    };
    IntrusiveRefCntPtr<OutputBackend> cloneImpl() const override {
      return const_cast<NullOutputBackend *>(this);
    }
    Expected<std::unique_ptr<OutputFileImpl>>
    createFileImpl(StringRef Path, Optional<OutputConfig>) override {
      return std::make_unique<NullOutput>();
    }
  };

  return makeIntrusiveRefCnt<NullOutputBackend>();
}

static OutputConfig
applySettings(Optional<OutputConfig> &&Config,
              const OnDiskOutputBackend::OutputSettings &Settings) {
  if (!Config)
    Config = Settings.DefaultConfig;
  if (Settings.DisableTemporaries)
    Config->setNoAtomicWrite();
  if (Settings.DisableRemoveOnSignal)
    Config->setNoCrashCleanup();
  return *Config;
}

namespace {
class OnDiskOutputFile final : public OutputFileImpl {
public:
  Error keep() override;
  Error discard() override;
  raw_pwrite_stream &getOS() override {
    assert(FileOS && "Expected valid file");
    if (BufferOS)
      return *BufferOS;
    return *FileOS;
  }

  /// Attempt to open a temporary file for \p OutputPath.
  ///
  /// This tries to open a uniquely-named temporary file for \p OutputPath,
  /// possibly also creating any missing directories if \a
  /// OnDiskOutputConfig::UseTemporaryCreateMissingDirectories is set in \a
  /// Config.
  ///
  /// \post FD and \a TempPath are initialized if this is successful.
  Error tryToCreateTemporary(Optional<int> &FD);

  Error initializeFD(Optional<int> &FD);
  Error initializeStream();

  OnDiskOutputFile(StringRef OutputPath, Optional<OutputConfig> Config,
                   const OnDiskOutputBackend::OutputSettings &Settings)
      : Config(applySettings(std::move(Config), Settings)),
        OutputPath(OutputPath.str()) {}

  OutputConfig Config;
  const std::string OutputPath;
  Optional<std::string> TempPath;
  Optional<raw_fd_ostream> FileOS;
  Optional<buffer_ostream> BufferOS;
};
} // end namespace

static Error createDirectoriesOnDemand(StringRef OutputPath,
                                       OutputConfig Config,
                                       llvm::function_ref<Error()> CreateFile) {
  return handleErrors(CreateFile(), [&](std::unique_ptr<ECError> EC) {
    if (EC->convertToErrorCode() != std::errc::no_such_file_or_directory ||
        Config.getNoImplyCreateDirectories())
      return Error(std::move(EC));

    StringRef ParentPath = sys::path::parent_path(OutputPath);
    if (std::error_code EC = sys::fs::create_directories(ParentPath))
      return make_error<OutputError>(ParentPath, EC);
    return CreateFile();
  });
}

Error OnDiskOutputFile::tryToCreateTemporary(Optional<int> &FD) {
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

  return createDirectoriesOnDemand(OutputPath, Config, [&]() -> Error {
    int NewFD;
    if (std::error_code EC =
            sys::fs::createUniqueFile(TempPath, NewFD, TempPath))
      return make_error<TempFileOutputError>(TempPath, OutputPath, EC);

    if (Config.getCrashCleanup())
      sys::RemoveFileOnSignal(TempPath);

    this->TempPath = TempPath.str().str();
    FD.emplace(NewFD);
    return Error::success();
  });
}

Error OnDiskOutputFile::initializeFD(Optional<int> &FD) {
  assert(OutputPath != "-" && "Unexpected request for FD of stdout");

  // Disable temporary file for other non-regular files, and if we get a status
  // object, also check if we can write and disable write-through buffers if
  // appropriate.
  if (Config.getAtomicWrite()) {
    sys::fs::file_status Status;
    sys::fs::status(OutputPath, Status);
    if (sys::fs::exists(Status)) {
      if (!sys::fs::is_regular_file(Status))
        Config.setNoAtomicWrite();

      // Fail now if we can't write to the final destination.
      if (!sys::fs::can_write(OutputPath))
        return make_error<OutputError>(
            OutputPath,
            std::make_error_code(std::errc::operation_not_permitted));
    }
  }

  // If (still) using a temporary file, try to create it (and return success if
  // that works).
  if (Config.getAtomicWrite())
    if (!errorToBool(tryToCreateTemporary(FD)))
      return Error::success();

  // Not using a temporary file. Open the final output file.
  return createDirectoriesOnDemand(OutputPath, Config, [&]() -> Error {
    int NewFD;
    if (std::error_code EC = sys::fs::openFileForWrite(
            OutputPath, NewFD, sys::fs::CD_CreateAlways,
            Config.getText() ? sys::fs::OF_Text : sys::fs::OF_None))
      return errorCodeToOutputError(OutputPath, EC);
    FD.emplace(NewFD);

    if (Config.getCrashCleanup())
      sys::RemoveFileOnSignal(OutputPath);
    return Error::success();
  });
}

Error OnDiskOutputFile::initializeStream() {
  // Open the file stream.
  if (OutputPath == "-") {
    std::error_code EC;
    FileOS.emplace(OutputPath, EC);
    if (EC)
      return make_error<OutputError>(OutputPath, EC);
  } else {
    Optional<int> FD;
    if (Error E = initializeFD(FD))
      return E;
    FileOS.emplace(*FD, /*shouldClose=*/true);
  }

  // Buffer the stream if necessary.
  if (!FileOS->supportsSeeking() && !Config.getText())
    BufferOS.emplace(*FileOS);

  return Error::success();
}

Error OnDiskOutputFile::keep() {
  // Destroy the streams to flush them.
  BufferOS.reset();
  FileOS.reset();

  // Close the file descriptor and remove crash cleanup before exit.
  auto RemoveCrashCleanup = make_scope_exit([&]() {
    if (Config.getCrashCleanup())
      sys::DontRemoveFileOnSignal(TempPath ? *TempPath : OutputPath);
  });

  if (!TempPath)
    return Error::success();

  // Move temporary to the final output path and remove it if that fails.
  std::error_code RenameEC = sys::fs::rename(*TempPath, OutputPath);
  if (!RenameEC)
    return Error::success();

  (void)sys::fs::remove(*TempPath);
  return make_error<TempFileOutputError>(*TempPath, OutputPath, RenameEC);
}

Error OnDiskOutputFile::discard() {
  // Destroy the streams to flush them.
  BufferOS.reset();
  FileOS.reset();

  // Nothing on the filesystem to remove for stdout.
  if (OutputPath == "-")
    return Error::success();

  auto discardPath = [&](StringRef Path) {
    std::error_code EC = sys::fs::remove(Path);
    sys::DontRemoveFileOnSignal(Path);
    return EC;
  };

  // Clean up the file that's in-progress.
  if (!TempPath)
    return errorCodeToOutputError(OutputPath, discardPath(OutputPath));
  return errorCodeToTempFileOutputError(*TempPath, OutputPath,
                                        discardPath(*TempPath));
}

Error OnDiskOutputBackend::makeAbsolute(SmallVectorImpl<char> &Path) const {
  return errorCodeToOutputError(StringRef(Path.data(), Path.size()),
                                sys::fs::make_absolute(Path));
}

Expected<std::unique_ptr<OutputFileImpl>>
OnDiskOutputBackend::createFileImpl(StringRef Path,
                                    Optional<OutputConfig> Config) {
  SmallString<256> AbsPath;
  if (Path != "-") {
    AbsPath = Path;
    if (Error E = makeAbsolute(AbsPath))
      return std::move(E);
    Path = AbsPath;
  }

  auto File = std::make_unique<OnDiskOutputFile>(Path, Config, Settings);
  if (Error E = File->initializeStream())
    return std::move(E);

  return std::move(File);
}
