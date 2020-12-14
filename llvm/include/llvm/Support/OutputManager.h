//===- llvm/Support/OutputManager.h - Output management ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_OUTPUTMANAGER_H
#define LLVM_SUPPORT_OUTPUTMANAGER_H

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/simple_ilist.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
namespace vfs {

/// Configuration for output intent in \a OutputManager.
enum class ClientIntentOutputConfig {
  NeedsSeeking,    /// Require a stream that supports seeking.
  NeedsReadAccess, /// Expect to read the file after writing.

  // Keep this last.
  NumFlags,
};

/// Configuration for on-disk outputs in \a OutputManager.
enum class OnDiskOutputConfig {
  UseTemporary,
  UseTemporaryCreateMissingDirectories,
  OpenFlagText,
  RemoveFileOnSignal,
  UseWriteThroughBuffer,

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
  constexpr static UnderlyingType getValue(ClientIntentOutputConfig Flag) {
    // Put ClientIntentOutputConfig at the start.
    return getRaw(Flag);
  }

  /// Convert \c Flag to \c UnderlyingType and offset it correctly.
  constexpr static UnderlyingType getValue(OnDiskOutputConfig Flag) {
    // Put OnDiskOutputConfig after ClientIntentOutputConfig.
    return getRaw(Flag) + getValue(ClientIntentOutputConfig::NumFlags);
  }

public:
  /// Get the total number of flags in the combined enumeration.
  constexpr static unsigned getNumFlags() {
    return getRaw(ClientIntentOutputConfig::NumFlags) +
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

class OutputDestination;
class OutputBackend;

/// Opaque description of a compiler output that has been created.
class Output {
public:
  /// Close an output, finalizing its content and sending it to the configured
  /// \a OutputBackend. If \p ShouldErase is set to \c true, redirects to \a
  /// erase() instead.
  ///
  /// This destructs the stream if it still owns it and writes the output to
  /// its final destination(s). E.g., for \a OnDiskOutputBackend, any temporary
  /// files will get moved into place, or for \a InMemoryOutputBackend, the \a
  /// MemoryBuffer will be created and stored in the \a InMemoryFileSystem.
  ///
  /// \pre If \a takeOS() has been called, the stream should have been
  /// destructed before calling this function.
  /// \pre \a isOpen(); i.e., neither \a erase() nor \a close() has been
  /// called yet.
  Error close(bool ShouldErase = false);

  /// Erase the output. Called on destruction if \a isOpen().
  ///
  /// \pre \a isOpen(); i.e., neither \a erase() nor \a close() has been
  /// called yet.
  void erase();

  /// Check if \a erase() or \a close() has already been called.
  bool isOpen() const { return IsOpen; }

  /// Get a pointer to the output stream, if this object still owns it.
  raw_pwrite_stream *getOS() const { return OS.get(); }

  /// Take the output stream. This stream should be destructed before calling
  /// \a close().
  std::unique_ptr<raw_pwrite_stream> takeOS() { return std::move(OS); }

  /// Get the configuration for this output.
  OutputConfig getConfig() const { return Config; }

  /// Get the output path for this output.
  StringRef getPath() const { return Path; }

  /// Erases the output if it hasn't already been closed.
  ~Output();

  /// Constructor for an output. \p Dest can be \c nullptr, in which case
  /// an instance of \a raw_null_ostream is constructed to use as the
  /// stream.
  Output(StringRef Path, OutputConfig Config,
         std::unique_ptr<OutputDestination> Dest)
      : Path(Path.str()), Config(Config), Dest(std::move(Dest)) {
    initializeOS();
  }

private:
  /// Helper for \a Output::Output().
  void initializeOS();

  std::string Path;
  OutputConfig Config;

  /// Tracks whether the output is still open, before one of \a erase() or \a
  /// close() is called.
  bool IsOpen = true;

  /// Content buffer if the output destination requests one.
  Optional<SmallVector<char, 0>> Bytes;

  /// The target for this output, provided by \a OutputManager's \a
  /// OutputBackend.
  std::unique_ptr<OutputDestination> Dest;

  // Destroy before Dest and ContentBuffer since it could reference them in its
  // destructor.
  std::unique_ptr<raw_pwrite_stream> OS;
};

/// Manager for outputs, handling safe and atomic creation of outputs. \a
/// OutputManager accepts an arbitrary \a OutputBackend, which may write files
/// on disk, store buffers in an \a InMemoryFileSystem, send the bytes to other
/// arbitrary destinations, or some combination of the above.
class OutputManager {
public:
  /// Get the default congiuration for outputs.
  const OutputConfig &getDefaults() const { return Defaults; }
  OutputConfig &getDefaults() { return Defaults; }

  /// Open a new output for writing and add it to the list of tracked
  /// outputs.
  ///
  /// \param OutputPath - If given, the path to the output. "-" indicates
  /// stdout.
  /// \return An error, or a valid \a Output that is open and ready for
  /// content.
  Expected<std::unique_ptr<Output>>
  createOutput(StringRef OutputPath, PartialOutputConfig Overrides = {});

  /// Change the current OutputBackend, to be used for \a createOutput() until
  /// it's next changed.
  void setBackend(IntrusiveRefCntPtr<OutputBackend> Backend) {
    this->Backend = std::move(Backend);
  }

  /// Check whether there is current an \a OutputBackend installed.
  bool hasBackend() const { return bool(Backend); }

  /// Get a reference to the current backend.
  ///
  /// \pre \a hasBackend() is \c true.
  OutputBackend &getBackend() const {
    assert(hasBackend() && "OutputManager missing a backend?");
    return *Backend;
  }

  /// Take the current backend.
  ///
  /// \post \a hasBackend() is \c false.
  IntrusiveRefCntPtr<OutputBackend> takeBackend() { return std::move(Backend); }

  /// Initialize with a newly constructed \a OnDiskOutputBackend.
  OutputManager();

  /// Initialize with a custom backend.
  explicit OutputManager(IntrusiveRefCntPtr<OutputBackend> Backend)
      : Backend(std::move(Backend)) {}

private:
  /// Default configuration for files that aren't specifically configured. Can
  /// be modified with \a getDefaults().
  OutputConfig Defaults = {
      ClientIntentOutputConfig::NeedsSeeking,
      ClientIntentOutputConfig::NeedsReadAccess,
      OnDiskOutputConfig::UseTemporary,
      OnDiskOutputConfig::RemoveFileOnSignal,
  };

  IntrusiveRefCntPtr<OutputBackend> Backend;
};

/// RAII utility for temporarily swapping an \a OutputManager's backend.
class ScopedOutputManagerBackend {
public:
  /// Install \p TemporaryBackend as \p M's backend.
  ScopedOutputManagerBackend(OutputManager &M,
                             IntrusiveRefCntPtr<OutputBackend> TemporaryBackend)
      : M(M), OriginalBackend(M.takeBackend()) {
    M.setBackend(std::move(TemporaryBackend));
  }

  /// Restore the original backend to the \a OutputManager.
  ~ScopedOutputManagerBackend() { M.setBackend(std::move(OriginalBackend)); }

private:
  OutputManager &M;
  IntrusiveRefCntPtr<OutputBackend> OriginalBackend;
};

/// Backend interface for \a OutputManager. Its job is to generate \a
/// OutputDestination given an \a OutputPath and \c OutputConfig.
class OutputBackend : public RefCountedBase<OutputBackend> {
  virtual void anchor();

public:
  /// Create an output destination, suitable for initializing \a Output.
  /// Returning \c nullptr indicates that this backend is ignoring the output.
  ///
  /// \return A valid \a OutputDestination, \c nullptr, or an \a Error.
  virtual Expected<std::unique_ptr<OutputDestination>>
  createDestination(StringRef OutputPath, OutputConfig Config,
                    std::unique_ptr<OutputDestination> NextDest) = 0;

  virtual ~OutputBackend() = default;
};

/// Interface for managing the destination of an \a Output. Most users only
/// need to deal with \a Output.
///
/// \a OutputDestination's lifetime is expected to follow one of the following
/// "good" paths after construction:
/// - \a takeStream() is called and returns a valid stream. The caller writes
///   content to the stream, destructs it, and then calls \a
///   storeStreamedContent() is to store the content.
/// - \a takeStream() is called and returns \c nullptr. The caller collects the
///   content and calls \a storeContent() to store it.
/// - \a storeContent() is called without first calling \a takeStream(), as the
///   caller wants to pass in the completed content as a whole instead of
///   streaming.
///
/// If the destination is destructed before calling \a storeStreamedContent()
/// and \a storeContent(), this output will be cancelled and temporaries
/// cleaned up.
///
/// \a storeContent() is designed to allow output destinations to be chained,
/// passing content between them. \a ContentBuffer helps to manage the lifetime
/// of the content, copying data and constructing memory buffers only as
/// needed.
class OutputDestination {
  virtual void anchor();

public:
  /// Utility for holding completed content for an output.
  struct ContentBuffer {
    StringRef getPath() const {
      assert(Buffer && "Expected initialization?");
      return Buffer->getBufferIdentifier();
    }
    StringRef getBytes() const {
      assert(Buffer && "Expected initialization?");
      return Vector ? StringRef(Vector->begin(), Vector->size())
                    : Buffer->getBuffer();
    }

    /// Whether the content is currently owned by this utility.
    bool ownsContent() const { return Vector || OwnedBuffer; }

    /// Returns a valid \a MemoryBuffer. If \a ownsContent() is false,
    /// this buffer is a reference constructed on the fly using \a
    /// MemoryBuffer::getMemBuffer().
    ///
    /// \post \b ownsContent() is false.
    std::unique_ptr<MemoryBuffer> takeBuffer();

    /// Returns a valid \a MemoryBuffer that owns its bytes, or \c
    /// nullptr if \a ownsContent() is \c false.
    ///
    /// \post \b ownsContent() is false.
    std::unique_ptr<MemoryBuffer> takeOwnedBufferOrNull();

    /// Returns a valid \a MemoryBuffer that owns its bytes, or calls
    /// MemoryBuffer::getMemBufferCopy() if \a ownsContent() is \c false.
    ///
    /// \post \b ownsContent() is false.
    std::unique_ptr<MemoryBuffer> takeOwnedBufferOrCopy();

    /// Construct a reference to content owned by someone else.
    ///
    /// \post \b ownsContent() is false.
    ContentBuffer(MemoryBufferRef Buffer) : Buffer(Buffer) {
      assert(!this->Buffer->getBufferEnd()[0] && "Requires null terminator");
    }

    /// Construct a buffer named \p Identifier from \p Vector. This buffer is
    /// created lazily on the first call to \a takeBuffer() and friends using
    /// \a SmallVectorMemoryBuffer.
    ///
    /// \post \b ownsContent() is true, unless \c Vector.empty().
    /// \post \a getBytes() returns a range with the memory from \p Vector,
    /// unless \p Vector was empty or in small mode.
    ContentBuffer(SmallVectorImpl<char> &&Vector, StringRef Identifier)
        : Vector(std::move(Vector)) {
      finishConstructingFromVector(Identifier);
      assert(!Buffer->getBufferEnd()[0] && "Requires null terminator");
    }

    /// Store \p Buffer as the content, returned by the first call to \a
    /// takeBuffer() and friends.
    ///
    /// \pre \p Buffer is null-terminated.
    /// \post \b ownsContent() is true.
    ContentBuffer(std::unique_ptr<MemoryBuffer> Buffer)
        : Buffer(*Buffer), OwnedBuffer(std::move(Buffer)) {
      assert(!this->Buffer->getBufferEnd()[0] && "Requires null terminator");
    }

    ContentBuffer(ContentBuffer &&) = default;
    ContentBuffer &operator=(ContentBuffer &&) = default;

  private:
    ContentBuffer() = delete;
    ContentBuffer(const ContentBuffer &&) = delete;
    ContentBuffer &operator=(const ContentBuffer &&) = delete;

    /// Main body of constructor when initializing \a Vector.
    void finishConstructingFromVector(StringRef Identifier);

    /// Owned content stored in a vector.
    Optional<SmallVector<char, 0>> Vector;

    /// Always initialized; only optional to facilite the small vector
    /// constructor.
    Optional<MemoryBufferRef> Buffer;

    /// Owned content stored in a memory buffer.
    std::unique_ptr<MemoryBuffer> OwnedBuffer;
  };

  /// Take the output stream directly. Can return \c nullptr if this
  /// destination requires a content buffer.
  ///
  /// If this function is called and \c nullptr is NOT returned, then once the
  /// stream has been filled with content \a storeStreamedContent() should be
  /// called to finalize it.
  ///
  /// If \c nullptr is returned, \a storeContent() should be called instead of
  /// \a storeStreamedContent() with a filled content buffer.
  std::unique_ptr<raw_pwrite_stream> takeStream() {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(IsOpen);
    assert(!TookStream);
    TookStream = true;
#endif
    std::unique_ptr<raw_pwrite_stream> Stream;
    if (!Next)
      Stream = takeStreamImpl();
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    RequestedContentBuffer = !Stream;
#endif
    return Stream;
  }

  /// Store streamed content. Only valid to call if \a takeStream() was called
  /// and it did not return \c nullptr.
  Error storeStreamedContent() {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(IsOpen);
    assert(TookStream);
    assert(!Next);
    assert(!RequestedContentBuffer);
    IsOpen = false;
#endif
    return storeStreamedContentImpl();
  }

  /// Store \a Content in the output destination, and then forward it the next
  /// one in the chain, if any. Not valid to call if \a takeStream() was called
  /// and returned a stream.
  ///
  /// Calls \a storeContentImpl() with a reference to \p Content, then forwards
  /// it to \a Next.
  Error storeContent(ContentBuffer Content) {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(IsOpen);
    assert(!TookStream || RequestedContentBuffer);
    IsOpen = false;
#endif
    if (Error E = storeContentImpl(Content))
      return E;
    if (Next)
      return Next->storeContent(std::move(Content));
    return Error::success();
  }

protected:
  /// Override this to allow content to be written directly to a stream, rather
  /// than collected in a content buffer.
  virtual std::unique_ptr<raw_pwrite_stream> takeStreamImpl() {
    return nullptr;
  }

  /// If \a takeStreamImpl() is called and does not return \c nullptr, this
  /// will be called when content should be flushed.
  virtual Error storeStreamedContentImpl() {
    llvm_unreachable("override this when overriding takeStreamImpl");
  }

  /// All destinations must know how to handle a content buffer.
  virtual Error storeContentImpl(ContentBuffer &Content) = 0;

  /// All destinations must support being constructed in front of a chain
  /// of destinations.
  explicit OutputDestination(std::unique_ptr<OutputDestination> Next)
      : Next(std::move(Next)) {}

public:
  virtual ~OutputDestination() = default;

private:
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
  bool IsOpen = true;
  bool TookStream = false;
  bool RequestedContentBuffer = false;
#endif

  std::unique_ptr<OutputDestination> Next;
};

/// Base class for OutputManager errors.
class OutputError : public ErrorInfo<OutputError> {
  void anchor() override;

public:
  StringRef getOutputPath() const { return OutputPath; }
  std::error_code getErrorCode() const { return EC; }

  std::error_code convertToErrorCode() const override { return getErrorCode(); }

  // Used by ErrorInfo::classID.
  static char ID;

protected:
  OutputError(StringRef OutputPath, std::error_code EC)
      : OutputPath(OutputPath), EC(EC) {
    assert(EC && "Cannot create OutputError from success EC");
  }

private:
  std::string OutputPath;
  std::error_code EC;
};

/// The output already exists in filesystem and it cannot be overwritten.
class CannotOverwriteExistingOutputError final
    : public ErrorInfo<CannotOverwriteExistingOutputError, OutputError> {
  friend Error createCannotOverwriteExistingOutputError(StringRef OutputPath);

  void anchor() override;

public:
  void log(raw_ostream &OS) const override {
    OS << "'" << getOutputPath() << "' already exists in-memory";
  }

  // Used by ErrorInfo::classID.
  static char ID;

private:
  CannotOverwriteExistingOutputError(StringRef OutputPath)
      : ErrorInfo<CannotOverwriteExistingOutputError, OutputError>(
            OutputPath,
            std::make_error_code(std::errc::device_or_resource_busy)) {}
};

inline Error createCannotOverwriteExistingOutputError(StringRef OutputPath) {
  return Error(std::unique_ptr<CannotOverwriteExistingOutputError>(
      new CannotOverwriteExistingOutputError(OutputPath)));
}

/// A temporary file could not be renamed to the final output on disk.
class OnDiskOutputRenameTempError final
    : public ErrorInfo<OnDiskOutputRenameTempError, OutputError> {
  friend Error createOnDiskOutputRenameTempError(StringRef TempPath,
                                                 StringRef OutputPath,
                                                 std::error_code EC);

  void anchor() override;

public:
  void log(raw_ostream &OS) const override {
    assert(getErrorCode());
    OS << "'" << TempPath << "' could not be renamed to ";
    OS << "'" << getOutputPath() << "': ";
    OS << getErrorCode().message();
  }

  StringRef getTempPath() const { return TempPath; }

  // Used by ErrorInfo::classID.
  static char ID;

private:
  OnDiskOutputRenameTempError(StringRef TempPath, StringRef OutputPath,
                              std::error_code EC)
      : ErrorInfo<OnDiskOutputRenameTempError, OutputError>(OutputPath, EC),
        TempPath(TempPath) {
    assert(EC && "Cannot create OnDiskOutputRenameTempError from success EC");
  }

  std::string TempPath;
};

inline Error createOnDiskOutputRenameTempError(StringRef TempPath,
                                               StringRef OutputPath,
                                               std::error_code EC) {
  return Error(std::unique_ptr<OnDiskOutputRenameTempError>(
      new OnDiskOutputRenameTempError(TempPath, OutputPath, EC)));
}

class OnDiskOutputBackend : public OutputBackend {
  void anchor() override;

public:
  Expected<std::unique_ptr<OutputDestination>>
  createDestination(StringRef OutputPath, OutputConfig Config,
                    std::unique_ptr<OutputDestination> NextDest) override;

  /// Big enough that mmap won't use up too much address space.
  static constexpr unsigned MinimumSizeToReturnWriteThroughBuffer = 4 * 4096;
};

/// Create a backend that ignores all output.
IntrusiveRefCntPtr<OutputBackend> makeNullOutputBackend();

/// Create a backend that mirrors content between \a Backend1 and \a
/// Backend2.
///
/// Both backends are asked to create destinations for each output. If neither
/// is ignore the content, then \a Backend1 will receive it before \a Backend2.
IntrusiveRefCntPtr<OutputBackend>
makeMirroringOutputBackend(IntrusiveRefCntPtr<OutputBackend> Backend1,
                           IntrusiveRefCntPtr<OutputBackend> Backend2);

/// A backend for storing outputs in an instance of \a InMemoryFileSystem.
class InMemoryOutputBackend : public OutputBackend {
  void anchor() override;

public:
  Expected<std::unique_ptr<OutputDestination>>
  createDestination(StringRef OutputPath, OutputConfig Config,
                    std::unique_ptr<OutputDestination> NextDest) override;

  /// Install an in-memory filesystem for writing outputs.
  ///
  /// Modify \a getDefaults() to turn on this use of \p FS by default,
  /// otherwise it will only be used for files with custom configuration.
  void setInMemoryFS(IntrusiveRefCntPtr<InMemoryFileSystem> FS) {
    assert(FS && "expected valid file-system");
    InMemoryFS = std::move(FS);
  }

  /// Get the installed in-memory filesystem, if any.
  InMemoryFileSystem &getInMemoryFS() const { return *InMemoryFS; }

  bool shouldStoredBufferBeOwned() const { return ShouldStoredBufferBeOwned; }
  void setShouldStoredBufferBeOwned(bool ShouldOwn) {
    ShouldStoredBufferBeOwned = ShouldOwn;
  }

  InMemoryOutputBackend(IntrusiveRefCntPtr<InMemoryFileSystem> FS) {
    setInMemoryFS(std::move(FS));
  }

private:
  bool ShouldStoredBufferBeOwned = false;

  /// In-memory filesystem for writing outputs to.
  IntrusiveRefCntPtr<InMemoryFileSystem> InMemoryFS;
};

/// Create an adaptor backend that filters the outputs that written to the
/// underlying backend.
IntrusiveRefCntPtr<OutputBackend> makeFilteringOutputBackend(
    IntrusiveRefCntPtr<OutputBackend> UnderlyingBackend,
    unique_function<bool(StringRef, OutputConfig)> Filter);

/// Get a stream-based backend that plugs in using callbacks.
IntrusiveRefCntPtr<OutputBackend> makeCallbackStreamOutputBackend(
    unique_function<Expected<std::unique_ptr<raw_pwrite_stream>>(StringRef,
                                                                 OutputConfig)>
        OnOpen,
    unique_function<Error(StringRef)> OnClose = nullptr,
    unique_function<void(StringRef)> OnErase = nullptr);

} // namespace vfs
} // namespace llvm

#endif // LLVM_SUPPORT_OUTPUTMANAGER_H
