//===- OutputBackend.h - Output virtualization ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_OUTPUTBACKEND_H
#define LLVM_SUPPORT_OUTPUTBACKEND_H

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/simple_ilist.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/OutputConfig.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
namespace vfs {

class OutputBackend;

/// A virtualized output file that can write to a specific backend.
class OutputFile : public raw_pwrite_stream {
  virtual void anchor();

public:
  /// Keep an output, finalizing its content and sending it to the \a
  /// OutputBackend. Returns \a Error::success() unless an output cannot be
  /// written.
  ///
  /// This writes the output to its final destination(s). E.g., for \a
  /// OnDiskOutputBackend, any temporary files will get moved into place.
  ///
  /// \pre Neither \a keep() nor \a discard() has been called yet.
  Error keep();

  /// Discard an output, cleaning up any temporary state. Returns \a
  /// Error::success() unless cleanup fails.
  ///
  /// This discards the output. E.g., for \a OnDiskOutputBackend, any temporary
  /// files will get erased.
  ///
  /// \pre Neither \a keep() nor \a discard() has been called yet.
  Error discard();

protected:
  virtual Error keepImpl() = 0;
  virtual Error discardImpl() { return Error::success(); }

  Error checkBeforeClose(bool IsKeep);

public:
  /// Check if \a keep() or \a discard() has already been called.
  bool isOpen() const { return IsOpen; }

  StringRef getPath() const { return Path; }

  /// Check if this is known to be /dev/null. Allows any derived \a OutputFile
  /// to skip unnecessary work.
  virtual bool isNull() const { return false; }

  /// Check if this file is capturing the output.
  virtual bool isCapturing() const { return false; }

  /// Create a proxy stream that can be passed to consumers that expect to own
  /// the stream. Must be destroyed before calling \a keep().
  std::unique_ptr<raw_pwrite_stream> createProxyStream();

  virtual ~OutputFile();

  OutputFile(OutputFile &&O) = delete;
  OutputFile &operator=(OutputFile &&O) = delete;

protected:
  explicit OutputFile(StringRef Path) : Path(Path.str()) {}

private:
  /// Output path.
  std::string Path;

  /// Tracks whether the output is still open, before one of \a keep() or \a
  /// discard() is called.
  bool IsOpen = true;

  /// Tracks whether there is a proxy for this stream. Guards against
  /// flush-after-close.
  bool HasOpenProxy = false;
};

class CapturedOutputFile : public OutputFile {
  virtual void anchor();

public:
  using OutputFile::keep;
  Error keep(llvm::function_ref<void (MemoryBufferRef)> CaptureBufferRef);
  Error keep(llvm::function_ref<void (std::unique_ptr<MemoryBuffer>)> CaptureBuffer,
             bool RequiresNullTerminator);

protected:
  using OutputFile::keepImpl;
  virtual Error keepImpl(llvm::function_ref<void (MemoryBufferRef)> CaptureBufferRef) = 0;
  virtual Error keepImpl(llvm::function_ref<void (std::unique_ptr<MemoryBuffer>)> CaptureBuffer,
                         bool RequiresNullTerminator) {
    (void)RequiresNullTerminator;
    return keepImpl([CaptureBuffer](MemoryBufferRef Ref) {
                    CaptureBuffer(MemoryBuffer::getMemBufferCopy(Ref.getBuffer(),
                                                                 Ref.getBufferName())));
    });
  }

private:
  Error close();

public:
  bool isCapturing() const override { return true; }

  /// Downcast an existing OutputFile, or create one that can be captured.
  static std::unique_ptr<CapturedOutputFile> createOrCast(std::unique_ptr<OutputFile> File);

protected:
  explicit CapturedOutputFile(StringRef Path) : OutputFile(Path.str()) {}
};

/// A implementation helper for an \a OutputFile that needs to buffer its
/// output.
class BufferedOutputFile : public raw_pwrite_stream_proxy_adaptor<CapturedOutputFile> {
  using AdaptorT = raw_pwrite_stream_proxy_adaptor<CapturedOutputFile>;

  virtual void anchor();

public:
  explicit BufferedOutputFile(StringRef Path) : AdaptorT(Path), VectorOS(Bytes) {
    setProxiedOS(&VectorOS);
  }

protected:
  Error keepImpl() final;
  Error keepImpl(llvm::function_ref<void (MemoryBufferRef)> CaptureBufferRef) final;
  Error keepImpl(llvm::function_ref<void (std::unique_ptr<MemoryBuffer>)> CaptureBuffer,
                 bool RequiresNullTerminator) final;

  virtual Error keepImpl(StringRef Bytes) {}
  virtual Error keepImpl(SmallVectorImpl<char> &&Storage) {
    return keepImpl(StringRef(Storage.begin(), Storage.end()));
  }

private:
  SmallVector<char, 0> Bytes;
  raw_svector_ostream VectorOS;
};

class OutputDirectory;

/// Backend interface for \a OutputBackend. Its job is to generate \a
/// OutputFileImpl given an \a OutputPath and \c OutputConfig.
class OutputBackend : public RefCountedBase<OutputBackend> {
  virtual void anchor();

public:
  Expected<std::unique_ptr<OutputFile>>
  createFile(const Twine &Path, OutputConfig Config = OutputConfig());

  /// Get the \a OutputDirectory for \p Path, which is an \a OutputBackend that
  /// uses \c Path as the current working directory. \p Path does not need to
  /// exist. This only fails when the call to \a resolveOutputPath() fails.
  Expected<IntrusiveRefCntPtr<OutputDirectory>>
  getDirectory(const Twine &Path) const;

  /// Crete the \a OutputDirectory for \p Path, which is an \a OutputBackend
  /// that uses \c Path as the current working directory.
  ///
  /// Not all backends have a concept of directories existing without files in
  /// them.
  Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createDirectory(const Twine &Path, OutputConfig Config = OutputConfig());

  /// Create a unique file.
  ///
  /// Not all backends have access to an input filesystem.
  Expected<std::unique_ptr<OutputFile>>
  createUniqueFile(const Twine &Model, OutputConfig Config = OutputConfig());

  Expected<std::unique_ptr<OutputFile>>
  createUniqueFile(const Twine &Prefix, StringRef Suffix,
                   OutputConfig Config = OutputConfig()) {
    return createUniqueFile(Prefix + "-%%%%%%%%" + Suffix, Config);
  }

  Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createUniqueDirectory(const Twine &Prefix,
                        OutputConfig Config = OutputConfig());

  sys::path::Style getPathStyle() const { return PathStyle; }

  /// A non-binding request to resolve \p Path. Output backends with a working
  /// directory will typically make relative paths absolute here. For example,
  /// see \a OutputDirectoryAdaptorBase::resolveOutputPathImpl().
  ///
  /// Called on all paths before forwarding to \a createFileImpl(),
  /// \a createUniqueFileImpl(), \a createUniqueDirectoryImpl(), or
  /// \a createDirectoryImpl().
  virtual Expected<StringRef>
  resolveOutputPath(const Twine &Path,
                    SmallVectorImpl<char> &PathStorage) const;

protected:
  /// Create an \a OutputDirectory without checking if the directory exists.
  virtual IntrusiveRefCntPtr<OutputDirectory>
  getDirectoryImpl(StringRef ResolvedPath) const;

  /// Create an output. Returning \c nullptr will cause the output to be
  /// ignored.  Implementations should not call \a initialize(); that's done by
  /// \a createFile().
  ///
  /// \return A valid \a OutputFile, \c nullptr, or an \a Error.
  virtual Expected<std::unique_ptr<OutputFile>>
  createFileImpl(StringRef ResolvedPath, OutputConfig Config) = 0;

  virtual Expected<std::unique_ptr<OutputFile>>
  createUniqueFileImpl(StringRef ResolvedModel, OutputConfig Config) = 0;

  /// Default implementation assumes that directory creation is a no-op, and
  /// returns a just-constructed \a OutputDirectory with \p ResolvedPath.
  virtual Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createDirectoryImpl(StringRef ResolvedPath, OutputConfig) {
    return getDirectory(ResolvedPath);
  }

  virtual Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createUniqueDirectoryImpl(StringRef ResolvedPrefix, OutputConfig Config) = 0;

public:
  OutputBackend() = delete;
  explicit OutputBackend(sys::path::Style PathStyle) : PathStyle(PathStyle) {}

  virtual ~OutputBackend() = default;

private:
  sys::path::Style PathStyle;
};

template <class OutputFileT = OutputFile>
class OutputFileProxyAdaptor : public raw_pwrite_stream_proxy_adaptor<OutputFileT> {
  using RawPwriteStreamAdaptorT = raw_pwrite_stream_proxy_adaptor<OutputFileT>;
protected:
  virtual Error keepImpl() { return getProxiedOS().keep(); }
  virtual Error discardImpl() { return getProxiedOS().discard(); }

  OutputFileProxyAdaptor() = default;
  explicit OutputFileProxyAdaptor(OutputFile *OS) : RawPwriteStreamAdaptorT(OS) {}

  raw_pwrite_stream &getProxiedOS() const {
    return static_cast<OutputFile &>(RawPwriteStreamAdaptorT::getProxiedOS());
  }
  void setProxiedOS(OutputFile &OS) {
    RawPwriteStreamAdaptorT::setProxiedOS(OS);
  }
};

class OutputFileProxy : public OutputFileProxyAdaptor<> {
  void anchor() override;

public:
  OutputFileProxy(OutputFile &OS) : OutputFileProxyAdaptor<>(&OS) {}
};

/// Helper class for creating proxies of another backend.
class ProxyOutputBackend : public OutputBackend {
protected:
  Expected<StringRef>
  resolveOutputPath(const Twine &Path,
                    SmallVectorImpl<char> &PathStorage) const override {
    return UnderlyingBackend->resolveOutputPath(Path, PathStorage);
  }

  Expected<std::unique_ptr<OutputFile>>
  createFileImpl(StringRef OutputPath, OutputConfig Config) override {
    return UnderlyingBackend->createFile(OutputPath, Config);
  }

  Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createDirectoryImpl(StringRef ResolvedPath, OutputConfig Config) override {
    return UnderlyingBackend->createDirectory(ResolvedPath, Config);
  }

  Expected<std::unique_ptr<OutputFile>>
  createUniqueFileImpl(StringRef ResolvedModel, OutputConfig Config) override {
    return UnderlyingBackend->createUniqueFile(ResolvedModel, Config);
  }

  Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createUniqueDirectoryImpl(StringRef ResolvedPrefix,
                            OutputConfig Config) override {
    return UnderlyingBackend->createUniqueDirectory(ResolvedPrefix, Config);
  }

  OutputBackend &getUnderlyingBackend() { return *UnderlyingBackend; }

public:
  ProxyOutputBackend(IntrusiveRefCntPtr<OutputBackend> UnderlyingBackend)
      : OutputBackend(UnderlyingBackend->getPathStyle()),
        UnderlyingBackend(std::move(UnderlyingBackend)) {
    assert(this->UnderlyingBackend && "Expected valid underlying backend");
  }

private:
  IntrusiveRefCntPtr<OutputBackend> UnderlyingBackend;
};

class OutputDirectoryAdaptorBase {
protected:
  /// If \p Path is relative and has a compatible root name with \a Directory,
  /// prepend \a Directory to make it absolute. Otherwise, do nothing.
  Expected<StringRef> resolveOutputPathImpl(const Twine &Path,
                                            SmallVectorImpl<char> &PathStorage,
                                            sys::path::Style PathStyle) const;

public:
  OutputDirectoryAdaptorBase(const Twine &Directory)
      : Directory(Directory.str()) {}

  StringRef getPath() const { return Directory; }

private:
  std::string Directory;
};

/// An adaptor to add a fixed working directory to an output backend.
template <class BaseBackendT = OutputBackend>
class OutputDirectoryAdaptor : public BaseBackendT, OutputDirectoryAdaptorBase {
protected:
  using OutputDirectoryAdaptorType = OutputDirectoryAdaptor;

public:
  Expected<StringRef>
  resolveOutputPath(const Twine &Path,
                    SmallVectorImpl<char> &PathStorage) const override {
    return OutputDirectoryAdaptorBase::resolveOutputPathImpl(
        Path, PathStorage, BaseBackendT::getPathStyle());
  }

  using OutputDirectoryAdaptorBase::getPath;

  template <class... ArgsT>
  OutputDirectoryAdaptor(const Twine &DirectoryPath, ArgsT &&... Args)
      : BaseBackendT(std::forward<ArgsT>(Args)...),
        OutputDirectoryAdaptorBase(DirectoryPath) {}
};

/// An output backend proxy with a fixed workign directory.
class OutputDirectory : public OutputDirectoryAdaptor<ProxyOutputBackend> {
  virtual void anchor() override;

public:
  /// Construct an output directory for a given backend. Does not check or
  /// modify \p DirectoryPath, just assumes it's valid and absolute-enough.
  ///
  /// Most users will want \a OutputBackend::getDirectory(), which first
  /// resolves the working directory, or \a OutputBackend::createDirectory(),
  /// which also explicit requests creating it.
  OutputDirectory(IntrusiveRefCntPtr<OutputBackend> UnderlyingBackend,
                  const Twine &DirectoryPath)
      : OutputDirectoryAdaptorType(DirectoryPath,
                                   std::move(UnderlyingBackend)) {}
};

/// A helper class for creating unique files and directories.
class StableUniqueEntityHelper {
public:
  StringRef getNext(StringRef Model, SmallVectorImpl<char> &PathStorage);

  Expected<std::unique_ptr<OutputFile>>
  createStableUniqueFile(OutputBackend &Backend, StringRef Model,
                         OutputConfig Config);

  Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createStableUniqueDirectory(OutputBackend &Backend, StringRef Model,
                              OutputConfig Config);

private:
  StringMap<unsigned> Models;
};

/// An adaptor to create a default implementation of \a
/// OutputBackend::createUniqueFileImpl() and \a
/// OutputBackend::createUniqueDirectoryImpl().
template <class BaseBackendT = OutputBackend>
class StableUniqueEntityAdaptor : public BaseBackendT,
                                  StableUniqueEntityHelper {
protected:
  using StableUniqueEntityAdaptorType = StableUniqueEntityAdaptor;

  Expected<std::unique_ptr<OutputFile>>
  createUniqueFileImpl(StringRef Model, OutputConfig Config) override {
    return this->createStableUniqueFile(*this, Model, Config);
  }

  Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createUniqueDirectoryImpl(StringRef Model, OutputConfig Config) override {
    return this->createStableUniqueDirectory(*this, Model, Config);
  }

public:
  template <class... ArgsT>
  StableUniqueEntityAdaptor(ArgsT &&... Args)
      : BaseBackendT(std::forward<ArgsT>(Args)...) {}
};

/// Interface for managing the destination of an \a OutputFile. Most users only
/// need to deal with \a OutputFile.
///
/// \a OutputFileImpl's lifetime is expected to follow one of the following
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
class BufferedOutputFile::ContentBuffer {
public:
  StringRef getBytes() const { return Bytes; }

  /// Whether the content is currently owned by this utility.
  bool ownsContent() const { return Vector || OwnedBuffer; }

  /// Returns a valid \a MemoryBuffer. If \a ownsContent() is false,
  /// this buffer is a reference constructed on the fly using \a
  /// MemoryBuffer::getMemBuffer().
  ///
  /// \post \b ownsContent() is false.
  std::unique_ptr<MemoryBuffer> takeBuffer(StringRef Identifier);

  /// Returns a valid \a MemoryBuffer that owns its bytes, or \c
  /// nullptr if \a ownsContent() is \c false.
  ///
  /// \post \b ownsContent() is false.
  std::unique_ptr<MemoryBuffer> takeOwnedBufferOrNull(StringRef Identifier);

  /// Returns a valid \a MemoryBuffer that owns its bytes, or calls
  /// MemoryBuffer::getMemBufferCopy() if \a ownsContent() is \c false.
  ///
  /// \post \b ownsContent() is false.
  std::unique_ptr<MemoryBuffer> takeOwnedBufferOrCopy(StringRef Identifier);

  /// Construct a reference to content owned by someone else.
  ///
  /// \post \b ownsContent() is false.
  ContentBuffer(StringRef Bytes) : Bytes(Bytes) {
    assert(!this->Bytes.end()[0] && "Requires null terminator");
  }

  /// Construct a buffer named \p Identifier from \p Vector. This buffer is
  /// created lazily on the first call to \a takeBuffer() and friends using
  /// \a SmallVectorMemoryBuffer.
  ///
  /// \post \b ownsContent() is true, unless \c Vector.empty().
  /// \post \a getBytes() returns a range with the memory from \p Vector,
  /// unless \p Vector was empty or in small mode.
  ContentBuffer(SmallVectorImpl<char> &&Vector) : Vector(std::move(Vector)) {
    finishConstructingFromVector();
    assert(!Bytes.end()[0] && "Requires null terminator");
  }

  /// Store \p Buffer as the content, returned by the first call to \a
  /// takeBuffer() and friends.
  ///
  /// \pre \p Buffer is null-terminated.
  /// \post \b ownsContent() is true.
  ContentBuffer(std::unique_ptr<MemoryBuffer> Buffer)
      : Bytes(Buffer->getBuffer()), OwnedBuffer(std::move(Buffer)) {
    assert(!Bytes.end()[0] && "Requires null terminator");
  }

  ContentBuffer(ContentBuffer &&) = default;
  ContentBuffer &operator=(ContentBuffer &&) = default;

private:
  ContentBuffer() = delete;
  ContentBuffer(const ContentBuffer &&) = delete;
  ContentBuffer &operator=(const ContentBuffer &&) = delete;

  /// Main body of constructor when initializing \a Vector.
  void finishConstructingFromVector();

  /// Reference to the bytes. Must be null-terminated.
  StringRef Bytes;

  /// Owned content stored in a vector.
  Optional<SmallVector<char, 0>> Vector;

  /// Owned content stored in a memory buffer.
  std::unique_ptr<MemoryBuffer> OwnedBuffer;
};

/// Base class for OutputBackend errors.
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
            OutputPath, std::make_error_code(std::errc::file_exists)) {}
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

protected:
  Expected<std::unique_ptr<OutputFile>>
  createFileImpl(StringRef ResolvedPath, OutputConfig Config) override;

  Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createDirectoryImpl(StringRef ResolvedPath, OutputConfig Config) override;

  Expected<std::unique_ptr<OutputFile>>
  createUniqueFileImpl(StringRef ResolvedModel, OutputConfig Config) override;

  Expected<IntrusiveRefCntPtr<OutputDirectory>>
  createUniqueDirectoryImpl(StringRef ResolvedPrefix,
                            OutputConfig Config) override;

public:
  /// Big enough that mmap won't use up too much address space.
  static constexpr unsigned MinimumSizeToReturnWriteThroughBuffer = 4 * 4096;

  /// On disk output settings.
  struct OutputSettings {
    /// Register output files to be deleted if a signal is received. Disabled
    /// for outputs with \a OutputConfig::getNoCrashCleanup().
    bool RemoveOnSignal = true;

    /// Use stable entity names for \a createUniqueFileImpl() and \a
    /// createUniqueDirectoryImpl().
    bool StableUniqueEntities = false;
    struct {
      bool Use = false;

      // If a write-through buffer is used, try to forward it. Disabled for
      // outputs with \a OutputConfig::getVolatile().
      bool Forward = true;
    } WriteThrough;
  };
  OutputSettings &getSettings() { return Settings; }
  const OutputSettings &getSettings() const { return Settings; }

  /// Get a view of this backend that has stable unique entity names.
  IntrusiveRefCntPtr<OutputBackend> createStableUniqueEntityView() {
    return makeIntrusiveRefCnt<StableUniqueEntityAdaptor<ProxyOutputBackend>>(
        this);
  }

  OnDiskOutputBackend() : OutputBackend(sys::path::Style::native) {}

private:
  OutputSettings Settings;
};

/// Create a backend that ignores all output.
IntrusiveRefCntPtr<OutputBackend>
makeNullOutputBackend(sys::path::Style PathStyle = sys::path::Style::native);

/// Create a backend that mirrors content between \a Backend1 and \a
/// Backend2.
///
/// Both backends are asked to create destinations for each output. \a Backend1
/// receives the content before \a Backend2.
IntrusiveRefCntPtr<OutputBackend>
makeMirroringOutputBackend(IntrusiveRefCntPtr<OutputBackend> Backend1,
                           IntrusiveRefCntPtr<OutputBackend> Backend2);

/// A backend for storing outputs in an instance of \a InMemoryFileSystem.
class InMemoryOutputBackend : public StableUniqueEntityAdaptor<> {
  void anchor() override;

protected:
  Expected<std::unique_ptr<OutputFile>>
  createFileImpl(StringRef OutputPath, OutputConfig Config) override;

  /// Get the installed in-memory filesystem, if any.
  InMemoryFileSystem &getInMemoryFS() const { return *FS; }

public:
  struct OutputSettings {
    bool FailIfExists = false;
    bool CopyUnownedBuffers = false;
  };
  OutputSettings &getSettings() { return Settings; }
  const OutputSettings &getSettings() const { return Settings; }

  InMemoryOutputBackend(IntrusiveRefCntPtr<InMemoryFileSystem> FS,
                        sys::path::Style PathStyle = sys::path::Style::native)
      : StableUniqueEntityAdaptorType(PathStyle), FS(std::move(FS)) {}

private:
  OutputSettings Settings;

  /// In-memory filesystem for writing outputs to.
  IntrusiveRefCntPtr<InMemoryFileSystem> FS;
};

/// Create an adaptor backend that filters the outputs that written to the
/// underlying backend. Outputs where \p Filter returns \c false will be
/// ignored.
IntrusiveRefCntPtr<OutputBackend> makeFilteringOutputBackend(
    IntrusiveRefCntPtr<OutputBackend> UnderlyingBackend,
    unique_function<bool(StringRef, OutputConfig)> Filter);

} // namespace vfs
} // namespace llvm

#endif // LLVM_SUPPORT_OUTPUTBACKEND_H
