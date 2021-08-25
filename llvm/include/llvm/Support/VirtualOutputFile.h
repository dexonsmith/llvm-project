//===- VirtualOutputFile.h - Output file virtualization ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_VIRTUALOUTPUTFILE_H
#define LLVM_SUPPORT_VIRTUALOUTPUTFILE_H

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualOutputError.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
namespace vfs {

class OutputFileImpl {
protected:
  virtual void anchor();

public:
  virtual ~OutputFileImpl() = default;

  virtual Error keep() = 0;
  virtual Error discard() = 0;
  virtual raw_pwrite_stream &getOS() = 0;
};

/// A virtualized output file that writes to a specific backend.
///
/// One of \a keep(), \a discard(), or \a discardOnDestroy() must be called
/// before destruction.
class OutputFile {
public:
  StringRef getPath() const { return Path; }

  /// Check if \a keep() or \a discard() has already been called.
  bool isOpen() const { return bool(Impl); }

  explicit operator bool() const { return isOpen(); }

  raw_pwrite_stream &getOS() {
    assert(isOpen() && "Expected open output stream");
    return Impl->getOS();
  }
  operator raw_ostream &() { return getOS(); }
  template <class T> raw_ostream &operator<<(T &&V) {
    return getOS() << std::forward<T>(V);
  }

  /// Keep an output. Errors if this fails or it's already closed.
  Error keep();

  /// Discard an output, cleaning up any temporary state. Errors if clean-up
  /// fails or it's already closed.
  Error discard();

  /// Discard the output when destroying it if it's still open, sending the
  /// result to \a Handler.
  void discardOnDestroy(unique_function<void(Error E)> Handler) {
    DiscardOnDestroyHandler = std::move(Handler);
  }

  OutputFile() = default;

  explicit OutputFile(const Twine &Path, std::unique_ptr<OutputFileImpl> Impl)
      : Path(Path.str()), Impl(std::move(Impl)) {
    assert(this->Impl && "Expected open output file");
  }

  ~OutputFile() { destroy(); }
  OutputFile(OutputFile &&O) { moveFrom(O); }
  OutputFile &operator=(OutputFile &&O) {
    destroy();
    return moveFrom(O);
  }

private:
  /// Destroy \a Impl. Reports fatal error if the file is open and there's no
  /// handler from \a discardOnDestroy().
  void destroy();
  OutputFile &moveFrom(OutputFile &O) {
    Path = std::move(O.Path);
    Impl = std::move(O.Impl);
    DiscardOnDestroyHandler = std::move(O.DiscardOnDestroyHandler);
    return *this;
  }

  std::string Path;
  std::unique_ptr<OutputFileImpl> Impl;
  unique_function<void(Error E)> DiscardOnDestroyHandler;
};

/// Update \p File to silently discard itself if it's still open when it's
/// destroyed.
inline void consumeDiscardOnDestroy(OutputFile &File) {
  File.discardOnDestroy(consumeError);
}

/// Update \p File to silently discard itself if it's still open when it's
/// destroyed.
inline Expected<OutputFile> consumeDiscardOnDestroy(Expected<OutputFile> File) {
  if (File)
    consumeDiscardOnDestroy(*File);
  return File;
}

} // namespace vfs
} // namespace llvm

#endif // LLVM_SUPPORT_VIRTUALOUTPUTFILE_H
