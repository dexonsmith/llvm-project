//===- OutputManager.h - Output management ------------- --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_OUTPUTMANAGER_H
#define LLVM_CLANG_BASIC_OUTPUTMANAGER_H

#include "clang/Basic/LLVM.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/simple_ilist.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

namespace clang {

/// Configuration for in-memory outputs in \a OutputManager.
enum class InMemoryOutputConfig {
  Enabled,
  ReuseWriteThroughBuffer,

  // Keep this last.
  NumFlags,
};

/// Configuration for on-disk outputs in \a OutputManager.
enum class OnDiskOutputConfig {
  Enabled,
  OpenFlagText,
  UseTemporary,
  UseTemporaryCreateMissingDirectories,
  RemoveFileOnSignal,
  UseWriteThroughWhenAlone,
  UseWriteThroughWhenReused,

  // Keep this last.
  NumFlags,
};

/// Combined configuration for outputs in \a OutputManager, combining the
/// different enum classes into a single enumeration.
class OutputConfigFlag {
  /// Convert \c Flag to a raw unsigned value.
  template <class FlagType> constexpr static unsigned getRaw(FlagType Flag) {
    return static_cast<unsigned>(Flag);
  }

public:
  using UnderlyingType = unsigned char;

private:
  /// Convert \c Flag to \c UnderlyingType and offset it correctly.
  constexpr static UnderlyingType getValue(InMemoryOutputConfig Flag) {
    // Put InMemoryOutputConfig at the start.
    return getRaw(Flag);
  }

  /// Convert \c Flag to \c UnderlyingType and offset it correctly.
  constexpr static UnderlyingType getValue(OnDiskOutputConfig Flag) {
    // Put OnDiskOutputConfig after InMemoryOutputConfig.
    return getRaw(Flag) + getValue(InMemoryOutputConfig::NumFlags);
  }

public:
  /// Get the total number of flags in the combined enumeration.
  constexpr static unsigned getNumFlags() {
    return getRaw(InMemoryOutputConfig::NumFlags) +
           getRaw(OnDiskOutputConfig::NumFlags);
  }

  /// Convert to the underlying type.
  constexpr explicit operator UnderlyingType() const { return Flag; }

  /// Construct the combined enumeration from any individual enum.
  template <class FlagType>
  constexpr OutputConfigFlag(FlagType Flag) : Flag(getValue(Flag)) {}

private:
  OutputConfigFlag() = delete;

  UnderlyingType Flag;
};

static_assert(std::numeric_limits<OutputConfigFlag::UnderlyingType>::max() >=
                  OutputConfigFlag::getNumFlags(),
              "Ran out of space for combined enums");

/// Shared code for \a PartialOutputConfig and \a OutputConfig.
class OutputConfigBase {
public:
  /// Don't use std::bitset since it's mostly not constexpr. If primitive types
  /// don't have enough bits, this can use a simple constexpr-friendly bitset.
  using BitsetType = unsigned short;

  /// Construct the combined enumeration from any individual enum.
  constexpr static BitsetType getBitset(OutputConfigFlag Flag) {
    static_assert(sizeof(BitsetType) <= sizeof(unsigned),
                  "Returned literal will overflow");
    return 1u << static_cast<OutputConfigFlag::UnderlyingType>(Flag);
  }

  constexpr OutputConfigBase() = default;
};

static_assert(sizeof(OutputConfigBase::BitsetType) * 8 >=
                  OutputConfigFlag::getNumFlags(),
              "Ran out of bits!");

/// Partial configuration for an output for use with \a OutputManager. Each
/// configuration flag can be \c true, \c false, or \a None.
class PartialOutputConfig : OutputConfigBase {
  constexpr void setBits(BitsetType Bits, Optional<bool> Value) {
    if (!Value) {
      Mask &= ~Bits;
      return;
    }

    Mask |= Bits;
    if (*Value)
      Values |= Bits;
    else
      Values &= ~(Bits);
  }

public:
  /// Check if anything is set.
  constexpr bool empty() const { return !Mask; }

  /// Check the configuration value for \c Flag, if any.
  constexpr Optional<bool> test(OutputConfigFlag Flag) const {
    if (Mask & getBitset(Flag))
      return Values & getBitset(Flag);
    return None;
  }

  /// Set the configuration value for \c Flag to \c Value (dropping the
  /// configuration if \c Value is \c None).
  constexpr PartialOutputConfig &set(OutputConfigFlag Flag,
                                     Optional<bool> Value = true) {
    setBits(getBitset(Flag), Value);
    return *this;
  }

  /// Set the configuration values for \c FlagsToChange to \c Value (dropping
  /// the configuration if \c Value is \c None).
  constexpr PartialOutputConfig &
  set(std::initializer_list<OutputConfigFlag> FlagsToChange,
      Optional<bool> Value = true) {
    BitsetType ChangeMask = 0;
    for (OutputConfigFlag Flag : FlagsToChange)
      ChangeMask |= getBitset(Flag);

    setBits(ChangeMask, Value);
    return *this;
  }

  /// Set the configuration value for \c Flag to \c false.
  constexpr PartialOutputConfig &reset(OutputConfigFlag Flag) {
    return set(Flag, false);
  }

  /// Set the configuration values for \c FlagsToChange to \c false.
  constexpr PartialOutputConfig &
  reset(std::initializer_list<OutputConfigFlag> FlagsToChange) {
    return set(FlagsToChange, false);
  }

  /// Drop the configuration for \c Flag (set it to None).
  constexpr PartialOutputConfig &drop(OutputConfigFlag Flag) {
    return set(Flag, None);
  }

  /// Drop the configuration for \c FlagsToChange (set them to None).
  constexpr PartialOutputConfig &
  drop(std::initializer_list<OutputConfigFlag> FlagsToChange) {
    return set(FlagsToChange, None);
  }

  /// Merge this partial configuration with \c Overrides, where the value in \c
  /// Overrides wins if one is \c true and the other \c false.
  constexpr PartialOutputConfig &applyOverrides(PartialOutputConfig Overrides) {
    Mask |= Overrides.Mask;
    Values |= Overrides.Mask & Overrides.Values;
    Values &= ~Overrides.Mask | Overrides.Values;
    return *this;
  }

  /// Merge this partial configuration with \c Defaults, where the existing
  /// value wins if one is \c true and the other \c false.
  constexpr PartialOutputConfig &applyDefaults(PartialOutputConfig Defaults) {
    return *this = Defaults.applyOverrides(*this);
  }

  /// Nothing is set.
  constexpr PartialOutputConfig() = default;

private:
  friend class OutputConfig;
  BitsetType Values = 0;
  BitsetType Mask = 0;
};

/// Full configuration for an output for use by the \a OutputManager. Each
/// configuration flag is either \c true or \c false.
class OutputConfig : OutputConfigBase {
public:
  /// Test whether there are no flags turned on.
  constexpr bool none() const { return !Flags; }

  /// Check the value for \c Flag.
  constexpr bool test(OutputConfigFlag Flag) const {
    return Flags & getBitset(Flag);
  }

  /// Set \c Flag to \c Value.
  constexpr OutputConfig &set(OutputConfigFlag Flag, bool Value = true) {
    if (Value)
      Flags |= getBitset(Flag);
    else
      Flags &= ~getBitset(Flag);
    return *this;
  }

  /// Set \c FlagsToChange to \c Value.
  constexpr OutputConfig &
  set(std::initializer_list<OutputConfigFlag> FlagsToChange,
      bool Value = true) {
    return applyOverrides(PartialOutputConfig().set(FlagsToChange, Value));
  }

  /// Set \c Flag to \c false.
  constexpr OutputConfig &reset(OutputConfigFlag Flag) {
    return set(Flag, false);
  }

  /// Set \c FlagsToChange to \c false.
  constexpr OutputConfig &
  reset(std::initializer_list<OutputConfigFlag> FlagsToChange) {
    return set(FlagsToChange, false);
  }

  /// Apply overrides from the partial configuration \p Overrides. Takes the
  /// value of any flag set in \c Overrides, leaving alone any flag where \p
  /// Overrides has \c None.
  constexpr OutputConfig &applyOverrides(PartialOutputConfig Overrides) {
    Flags |= Overrides.Mask & Overrides.Values;
    Flags &= ~Overrides.Mask | Overrides.Values;
    return *this;
  }

  /// Nothing is set.
  constexpr OutputConfig() = default;

  /// Set exactly the flags listed, leaving others turned off.
  constexpr OutputConfig(std::initializer_list<OutputConfigFlag> OnFlags) {
    set(OnFlags);
  }

private:
  BitsetType Flags = 0;
};

/// A temporary file could not be renamed to the final output on disk.
class OnDiskOutputRenameTempError final
    : public llvm::ErrorInfo<OnDiskOutputRenameTempError> {
  friend llvm::Error createOnDiskOutputRenameTempError(StringRef TempPath,
                                                       StringRef OutputPath,
                                                       std::error_code EC);

  void anchor() override;

public:
  void log(raw_ostream &OS) const override {
    assert(EC);
    OS << "'" << TempPath << "' could not be renamed to ";
    OS << "'" << OutputPath << "': ";
    OS << EC.message();
  }

  StringRef getTempPath() const { return TempPath; }
  StringRef getOutputPath() const { return OutputPath; }
  std::error_code getErrorCode() const { return EC; }
  std::error_code convertToErrorCode() const override { return EC; }

  // Used by ErrorInfo::classID.
  static char ID;

private:
  OnDiskOutputRenameTempError(StringRef TempPath, StringRef OutputPath,
                              std::error_code EC)
      : TempPath(TempPath), OutputPath(OutputPath), EC(EC) {
    assert(EC && "Cannot create OnDiskOutputRenameTempError from success EC");
  }

  std::string TempPath;
  std::string OutputPath;
  std::error_code EC;
};

inline llvm::Error createOnDiskOutputRenameTempError(StringRef TempPath,
                                                     StringRef OutputPath,
                                                     std::error_code EC) {
  return llvm::Error(std::unique_ptr<OnDiskOutputRenameTempError>(
      new OnDiskOutputRenameTempError(TempPath, OutputPath, EC)));
}

/// A temporary file could not be renamed to the final output on disk.
class OutputAlreadyRequestedError final
    : public llvm::ErrorInfo<OutputAlreadyRequestedError> {
  friend llvm::Error createOutputAlreadyRequestedError(StringRef OutputPath);

  void anchor() override;

public:
  void log(raw_ostream &OS) const override {
    OS << "'" << OutputPath << "' already requested";
  }

  StringRef getOutputPath() const { return OutputPath; }
  std::error_code getErrorCode() const {
    return std::make_error_code(std::errc::device_or_resource_busy);
  }
  std::error_code convertToErrorCode() const override { return getErrorCode(); }

  // Used by ErrorInfo::classID.
  static char ID;

private:
  OutputAlreadyRequestedError(StringRef OutputPath) : OutputPath(OutputPath) {}

  std::string OutputPath;
};

inline llvm::Error createOutputAlreadyRequestedError(StringRef OutputPath) {
  return llvm::Error(std::unique_ptr<OutputAlreadyRequestedError>(
      new OutputAlreadyRequestedError(OutputPath)));
}

/// Manager for outputs, handling safe and atomic creation of files.
class OutputManager {
  /// Buffer type for building up the output in-memory.
  using BufferType = llvm::SmallVector<char, 0>;

  struct InMemoryOutput {
    /// Store the content in \p InMemoryFS, taking advantage of \p Buffer if
    /// provided, else moving from \c Content.
    void storeContent(StringRef OutputPath, OutputConfig Config,
                      llvm::vfs::InMemoryFileSystem &InMemoryFS,
                      std::unique_ptr<llvm::MemoryBuffer> Buffer);

    /// Always set, but initially null to simplify construction.
    BufferType *Content = nullptr;
  };

  struct OnDiskOutput {
    /// Returns true if the final write uses a \a
    /// llvm::WriteThroughMemoryBuffer. This is only valid to call after \a
    /// initializeFile().
    bool useWriteThroughBuffer() const {
      assert(UseWriteThroughBuffer != None);
      return *UseWriteThroughBuffer;
    }

    /// Open a file on-disk for writing to \a OutputPath.
    ///
    /// This calls \a initializeFD() to initialize \a FD (see that
    /// documentation for more detail) and \a UseWriteThroughBuffer. Unless \a
    /// UseWriteThroughBuffer was determined to be \c true, this function will
    /// then initialize \a OS with a valid stream.
    llvm::Error initializeFile(StringRef OutputPath, OutputConfig Config);

    /// Take an open output stream.
    ///
    /// If the file does not support seeking and \a
    /// OnDiskOutputConfig::OpenFlagText is not set in \p Config, the file
    /// stream is interposed with \a llvm::buffer_unique_stream.
    std::unique_ptr<llvm::raw_pwrite_stream> takeStream(OutputConfig Config);

    /// Close a file after successfully collecting the output.
    ///
    /// The output content must have already been written somewhere, but
    /// exactly where depends on the configuration.
    ///
    /// - If \a WriteThroughMemoryBuffer is \c true, the output will be in \a
    ///   Content and \a FD has an open file descriptor. In that case, the file
    ///   will be completed by opening \a llvm::WriteThroughMemoryBuffer and
    ///   calling \a std::memcpy().
    /// - Else if the output is in \a Content, it will be written to \a OS,
    ///   which should be an already-open file stream, and the content will be
    ///   written there and the stream destroyed to flush it.
    /// - Else the content should already be in the file.
    ///
    /// Once the content is on-disk, if \a TempPath is set, this calls \a
    /// llvm::sys::rename() to move the file to \a OutputPath.
    llvm::Error closeFile(StringRef OutputPath,
                          std::unique_ptr<llvm::MemoryBuffer> &Buffer);

    /// Erase a file where the output should not be written to disk after all.
    ///
    /// If \a TempFile is set, erase it; otherwise, erase \p OutputPath.
    void eraseFile(StringRef OutputPath);

  private:
    /// Attempt to open a temporary file for \a OutputPath.
    ///
    /// This tries to open a uniquely-named temporary file for \a OutputPath,
    /// possibly also creating any missing directories if \a
    /// OnDiskOutputConfig::UseTemporaryCreateMissingDirectories is set in \a
    /// Config.
    ///
    /// \post FD and \a TempPath are initialized if this is successful.
    std::error_code tryToCreateTemporary(StringRef OutputPath,
                                         OutputConfig Config);

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
    ///   llvm::sys::fs::is_regular_file() returns \c false (such as a named
    ///   pipe). If \a tryToCreateTemporary() fails, this falls back to
    ///   no-temp-file mode. (Even if using a temporary file, \p OutputPath is
    ///   checked for write permission.)
    /// - \a OnDiskOutputConfig::RemoveFileOnSignal installs a signal handler
    ///   to remove the opened file.
    ///
    /// This function also initializes \a UseWriteThroughBuffer, using \a
    /// CanWriteThroughBufferBeReused to choose between \a
    /// OnDiskOutputConfig::UseWriteThroughWhenReused and \a
    /// OnDiskOutputConfig::UseWriteThroughWhenAlone, overridden with \c false
    /// if the file does not support write-through buffers.
    ///
    /// \post UseWriteThroughBuffer is set.
    /// \post FD is set unless OutputPath is \c "-" or on error.
    /// \post TempPath is set if a temporary file was opened successfully.
    llvm::Error initializeFD(StringRef OutputPath, OutputConfig Config);

  public:
    BufferType *Content = nullptr;
    bool CanWriteThroughBufferBeReused = false;

  private:
    Optional<bool> UseWriteThroughBuffer;
    std::unique_ptr<llvm::raw_fd_ostream> OS;
    Optional<std::string> TempPath;
    Optional<int> FD;
  };

  struct RequestedOutput {
    /// Copy the content to \c RequestedStream.
    void copyContent();

    /// Report that the file has been closed.
    void reportOnClose(StringRef OutputPath);

    bool needsContent() const { return RequestedStream != nullptr; }

    std::unique_ptr<llvm::raw_pwrite_stream> takeStream();

    RequestedOutput(
        llvm::unique_function<void(StringRef)> OnClose = nullptr,
        std::unique_ptr<llvm::raw_pwrite_stream> RequestedStream = nullptr)
        : OnClose(std::move(OnClose)),
          RequestedStream(std::move(RequestedStream)) {}

    /// Set if the request needs to forward content from a buffer.
    BufferType *Content = nullptr;

  private:
    /// Callback that runs at the end of \a OutputManager::closeOutput(), not
    /// called on eraseOutput.
    llvm::unique_function<void(StringRef)> OnClose;

    /// Requested stream, if any. If there is no other type of output (e.g.,
    /// not writing to disk or in-memory filesystem), the stream be returned by
    /// the call to \a OutputManager::createOutput(). Otherwise, the stream is
    /// kept until the call to \a OutputManager::closeOutput(), where the
    /// content is copied in bulk.
    std::unique_ptr<llvm::raw_pwrite_stream> RequestedStream;
  };

  struct Output {
    /// Configuration for the output.
    OutputConfig Config;

    Optional<BufferType> Content;
    Optional<InMemoryOutput> InMemory;
    Optional<OnDiskOutput> OnDisk;
    Optional<RequestedOutput> Request;

    Output() = delete;
    explicit Output(OutputConfig Config) : Config(Config) {}
  };

public:
  /// Handle for an output.
  class OutputHandle {
    StringRef getPath() const { return O.get().first(); }

  private:
    friend class OutputManager;
    OutputHandle(llvm::StringMapEntry<Output> &O) : O(O) {}
    OutputHandle() = delete;

    std::reference_wrapper<llvm::StringMapEntry<Output>> O;
  };

  /// Result from \a createOutput().
  struct CreateOutputResult {
    /// The output handle, needed for closing or erasing it later.
    OutputHandle Handle;

    /// The stream to write to to fill up the output.
    std::unique_ptr<llvm::raw_pwrite_stream> OS;

    CreateOutputResult(OutputHandle Handle,
                       std::unique_ptr<llvm::raw_pwrite_stream> OS)
        : Handle(Handle), OS(std::move(OS)) {}
  };

  /// Get the default congiuration for outputs.
  const OutputConfig &getDefaults() const { return Defaults; }
  OutputConfig &getDefaults() { return Defaults; }

  /// Install an in-memory filesystem for writing outputs.
  ///
  /// Modify \a getDefaults() to turn on this use of \p FS by default,
  /// otherwise it will only be used for files with custom configuration.
  void
  setInMemoryFileSystem(IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> FS) {
    InMemoryFS = std::move(FS);
  }

  /// Get the installed in-memory filesystem, if any.
  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem>
  getInMemoryFileSystem() const {
    return InMemoryFS;
  }

  /// Change the configuration for an output (before opening it).
  ///
  /// Apply \p Overrides to \p OutputPath's configuration (and return the
  /// result, as a convenience). This updates the configuration used in future
  /// calls to \a createOutput, and this output's configuration will not change
  /// in future changes to \a getDefaults().
  OutputConfig configureOutput(StringRef OutputPath,
                               PartialOutputConfig Overrides);

  /// Request the content of an output in a specific stream.
  ///
  /// Specify that \p OutputPath should be hooked up to \p OS in a future call
  /// \a createOutput. This does not on its own affect the configuration for
  /// the output; it may still be written to disk and/or stored in memory, but
  /// that can be customized with \p Overrides.
  ///
  /// \p OnClose registers a callback for when the output is closed (and \p OS
  /// is "ready").
  ///
  /// This function can fail if this output has already been requested. If the
  /// output has already been opened, this will NOT fail, this will queue for
  /// the next call.
  llvm::Error
  requestOutput(StringRef OutputPath,
                std::unique_ptr<llvm::raw_pwrite_stream> OS,
                llvm::unique_function<void(StringRef)> OnClose = nullptr,
                PartialOutputConfig Overrides = {});

  /// Request a notification when an output has been written without giving a
  /// stream.
  llvm::Error
  requestOutputCallback(StringRef OutputPath,
                        llvm::unique_function<void(StringRef)> OnClose,
                        PartialOutputConfig Overrides = {}) {
    return requestOutput(OutputPath, nullptr, std::move(OnClose), Overrides);
  }

  /// Open a new output for writing and add it to the list of tracked
  /// outputs.
  ///
  /// \param OutputPath - If given, the path to the output. "-" indicates
  /// stdout.
  llvm::Expected<CreateOutputResult>
  createOutput(StringRef OutputPath, PartialOutputConfig Overrides = {});

  /// Close Output, moving it to its final location as appropriate and cleaning
  /// up any temporaries.
  ///
  /// This invalidates the given handle.
  llvm::Error closeOutput(OutputHandle Output);

  /// Drop information about Output any the file specified by OutputPath and
  /// tries to erase any remnants from the filesystem.
  ///
  /// This invalidates the given handle.
  void eraseOutput(OutputHandle Output);

  /// Check if the handle is null. Exposed for ease of testing.
  bool isNullHandle(OutputHandle Output) const;

private:
  llvm::StringMapEntry<Output> &getOrCreateNullHandle();

  /// Add the \p OnClose callback, potentially merging it with another
  /// callback.
  void
  requestOutputCallbackImpl(Output &O,
                            llvm::unique_function<void(StringRef)> OnClose);

  /// Add (and return an iterator to) an Output to \a ActiveOutputs, possibly
  /// moving an existing one from \a Requests. If this returns
  /// ActiveOutputs.end(), this output is being ignored.
  llvm::Expected<llvm::StringMap<Output>::iterator>
  createActiveOutput(StringRef OutputPath, PartialOutputConfig Overrides);

  /// Default configuration for files that aren't specifically configured. Can
  /// be modified with \a getDefaults().
  OutputConfig Defaults = {
      InMemoryOutputConfig::ReuseWriteThroughBuffer,
      OnDiskOutputConfig::Enabled,
      OnDiskOutputConfig::UseTemporary,
      OnDiskOutputConfig::RemoveFileOnSignal,
      OnDiskOutputConfig::UseWriteThroughWhenReused,
  };

  /// Outputs that have been requested but not yet created.
  llvm::StringMap<Output> Requests;

  /// Outputs that are active; they have been created but not yet closed or
  /// erased.
  llvm::StringMap<Output> ActiveOutputs;

  /// Used for handles to objects that are being ignored.
  llvm::Optional<llvm::StringMap<Output>> NullMap;
  llvm::StringMapEntry<Output> *Null = nullptr;

  /// In-memory filesystem for writing outputs to.
  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFS;
};

class OutputContext {
public:
  OutputManager &getManager() const { return *Manager; }

  PartialOutputConfig &getDefaultOverrides() { return DefaultOverrides; }
  const PartialOutputConfig &getDefaultOverrides() const {
    return DefaultOverrides;
  }

  /// Create an output to be tracked in this context.
  llvm::Expected<std::unique_ptr<raw_pwrite_stream>>
  createOutput(StringRef OutputPath, PartialOutputConfig = {});

  /// Create an output to be tracked in this context, specifying a number of
  /// flags manually as Boolean parameters.
  llvm::Expected<std::unique_ptr<raw_pwrite_stream>>
  createOutput(StringRef OutputPath, bool Binary, bool RemoveFileOnSignal,
               bool UseTemporary, bool CreateMissingDirectories) {
    assert(
        (!CreateMissingDirectories || UseTemporary) &&
        "CreateMissingDirectories is only allowed when using temporary files");
    return createOutput(
        OutputPath,
        PartialOutputConfig()
            .set(OnDiskOutputConfig::OpenFlagText, !Binary)
            .set(OnDiskOutputConfig::RemoveFileOnSignal, RemoveFileOnSignal)
            .set(OnDiskOutputConfig::UseTemporary, UseTemporary)
            .set(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories,
                 CreateMissingDirectories));
  }

  /// Check whether any outputs are being tracked.
  bool hasTrackedOutputs() const { return !Outputs.empty(); }

  /// Close all tracked outputs, calling \a OutputManager::closeOutput() on
  /// each opened file.
  ///
  /// If \p ShouldErase is overridden to \c true, this calls \a
  /// OutputManager::eraseOutput() instead.
  ///
  /// \post hasTrackedOutputs() is \c false
  llvm::Error closeAllOutputs(bool ShouldErase = false);

  /// Erase all tracked outputs, OutputManager::eraseOutput() on each opened
  /// file.
  ///
  /// \post hasTrackedOutputs() is \c false
  void eraseAllOutputs();

  /// Create an output context for \p Manager.
  explicit OutputContext(std::shared_ptr<OutputManager> Manager,
                         std::string WorkingDir = "")
      : WorkingDir(std::move(WorkingDir)), Manager(Manager) {
    assert(Manager);
  }

  /// Create an output context with the same manager as \p Context. Copy its
  /// overrides and working directory.
  explicit OutputContext(const OutputContext &Context,
                         Optional<std::string> WorkingDir = None)
      : DefaultOverrides(Context.DefaultOverrides),
        WorkingDir(WorkingDir ? std::move(*WorkingDir)
                              : std::string(Context.WorkingDir)),
        Manager(Context.Manager) {}

  OutputContext(std::string WorkingDir = "")
      : OutputContext(std::make_shared<OutputManager>(),
                      std::move(WorkingDir)) {}

  ~OutputContext() {
    assert(Outputs.empty() && "Expected outputs to be erased or closed");
  }

private:
  PartialOutputConfig DefaultOverrides;
  std::string WorkingDir;
  std::shared_ptr<OutputManager> Manager;
  std::vector<OutputManager::OutputHandle> Outputs;
};

} // end namespace clang

#endif // LLVM_CLANG_BASIC_OUTPUTMANAGER_H
