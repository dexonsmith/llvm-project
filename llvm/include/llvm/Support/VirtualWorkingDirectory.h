//===- VirtualWorkingDirectory.h - Virtual working directory ------*- C++ -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_VIRTUALWORKINGDIRECTORY_H
#define LLVM_SUPPORT_VIRTUALWORKINGDIRECTORY_H

#include "llvm/Support/WorkingDirectoryState.h"
#include <bitset>
#include <string>

namespace llvm {
namespace vfs {

class FileSystem;

/// A virtual working directory, with support for tracking the live filesystem.
///
/// There are three modes.
///
/// 1. Virtual working directory.
///     - virtual WD
///     - sys::path::Style can be native, windows, or posix.
///     - makeAbsolute() is virtual (only)
///     - setCurrentWorkingDirectory() changes virtual WD
///
/// 2. Live filesystem.
///     - no virtual WD
///     - sys::path::Style is Style::native (always)
///     - makeAbsolute() calls sys::fs::make_absolute()
///     - setCurrentWorkingDirectory() calls sys::fs::set_current_path()
///
/// 3. Hybrid. (default)
///     - virtual WD
///     - sys::path::Style is Style::native (always)
///     - makeAbsolute() is virtual, but falls back to sys::fs::make_absolute()
///     - setCurrentWorkingDirectory() changes virtual WD
///
///
///
/// \a makeAbsolute() is thread-safe. \a setCurrentWorkingDirectory() is not.
/// This class can be shared between threads as long as no mutator is called.
///
/// This wraps an instance of \a WorkingDirectoryState, adding relative path
/// support for \a setCurrentWorkingDirectory() and configuration options for
/// initializing from (or deferring to) APIs in the \a sys::fs namespace.
///
/// By default, there is no virtual working directory. There are two ways to
/// initialize virtual state explicitly:
///
/// - \a initializeVirtualState() constructs an empty \a WorkingDirectoryState.
/// - \a initializeVirtualStateFromSnapshot() calls \a sys::fs::current_paths().
///
/// When there is no virtual state, or if the working directory for a relative
/// path is unknown, the behaviour of \a makeAbsolute() and \a
/// setCurrentWorkingDirectory() depend on \a Settings.
///
/// - Settings::getError() never uses APIs from \a sys::fs. It errors on calls
///   to \a makeAbsolute() and \a setCurrentWorkingDirectory() until the
///   virtual state is initialized.
/// - Settings::getVirtual(), the default, is like \a Settings::getError()
///   except that the first call to \a setCurrentWorkingDirectory() implies
///   initializeVirtualState().
/// - Settings::getInitLive() falls back on APIs from \a sys::fs instead of
///   erroring. The first call to \a setCurrentWorkingDirectory() implies \a
///   initializeVirtualStateFromSnapshot(), and \a makeAbsolute() calls
///   sys::path::make_absolute() if the current working directory is not valid
///   for the given path.
class WorkingDirectory : public llvm::ThreadSafeRefCountedBase<WorkingDirectory> {
public:
  using State = sys::path::WorkingDirectoryState;
  using Style = sys::path::Style;

  virtual Style getPathStyle() const { return Style::native; }
  virtual Error makeAbsolute(SmallVectorImpl<char> &Path) const = 0;
  virtual Error getCurrentWorkingDirectory(SmallVectorImpl<char> &Path) const = 0;
  virtual Expected<State> getCurrentWorkingDirectoryState() const = 0;
  virtual Error setCurrentWorkingDirectory(const Twine &Path) = 0;
};

/// Virtual.
class VirtualWorkingDirectory : public WorkingDirectory {
public:
  Style getPathStyle() const override;
  Error makeAbsolute(SmallVectorImpl<char> &Path) override;
  Error getCurrentWorkingDirectory(SmallVectorImpl<char> &Path) override;
  Expected<State> getCurrentWorkingDirectoryState() override;
  Error setCurrentWorkingDirectory(const Twine &Path) override;

  VirtualWorkingDirectory(Style PathStyle = Style::native);

private:
  WorkingDirectoryState WD;
};

// Live filesystem. Style::native.
class LiveWorkingDirectory final : public WorkingDirectory {
  Error makeAbsolute(SmallVectorImpl<char> &Path) override;
  Error getCurrentWorkingDirectory(SmallVectorImpl<char> &Path) override;
  Expected<State> getCurrentWorkingDirectoryState() override;
  Error setCurrentWorkingDirectory(const Twine &Path) override;
};

// Live vfs::FileSystem. Style matches the VFS.
class LiveWorkingDirectoryUsingVFS final : public WorkingDirectory {
  Error makeAbsolute(SmallVectorImpl<char> &Path) override;
  Error getCurrentWorkingDirectory(SmallVectorImpl<char> &Path) override;
  Expected<State> getCurrentWorkingDirectoryState() override;
  Error setCurrentWorkingDirectory(const Twine &Path) override;

private:
  IntrusiveRefCntPtr<vfs::FileSystem> FS;
};

// Virtual, with a fallback to another WD. Style matches the fallback.
class FallbackWorkingDirectory final : public VirtualWorkingDirectory {
public:
  Error makeAbsolute(SmallVectorImpl<char> &Path) override {
    if (errorToBool(VirtualWorkingDirectory::makeAbsolute(Path)))
      return FallbackWD.makeAbsolute(Path);
    return Error::success();
  }

  FallbackWorkingDirectory(const WorkingDirectory &Fallback) :
      VirtualWorkingDirectory(Fallback.getPathStyle()) {}

private:
  const WorkingDirectory &Fallback;
};

class VirtualWorkingDirectory {
public:
  using WorkingDirectoryState = sys::path::WorkingDirectoryState;
  using Style = sys::path::Style;

  enum Mode {
    Virtual, // Mutate and access the virtual WD.
    Live,    // Mutate and access the live filesystem.
    Hybrid,  // Mutate the virtual WD; access both.
  };

  Mode getMode() const { return M; }

  Style getPathStyle() const {
    return M == Virtual ? VirtualWD.getPathStyle() : Style::native;
  }

  /// Make \p Path absolute.
  Error makeAbsolute(SmallVectorImpl<char> &Path) const;

  /// Set the current working directory to \p Path.
  Error setCurrentWorkingDirectory(const Twine &Path);

  /// Get the current working directory.
  Error getCurrentWorkingDirectory(SmallVectorImpl<char> &Path) const;
  Expected<std::string> getCurrentWorkingDirectory() const;

  /// Access the virtual WD, if any.
  const WorkingDirectoryState *getVirtualWD() const {
    return M == Live ? nullptr : &VirtualWD;
  }

  /// Move the virtual WD, if any, into \p WD.
  void moveVirtualWDInto(WorkingDirectoryState &WD) {
    WD = M == Live ? WorkingDirectoryState() : std::move(VirtualWD);
  }

  /// Construct a hybrid working directory. Uses \p VirtualWD if provided.
  static VirtualWorkingDirectory
  createHybrid(WorkingDirectoryState VirtualWD = {}) {
    assert(VirtualWD.getPathStyle() == Style::native &&
           "Hybrid mode requires Style::native");
    return VirtualWorkingDirectory(Hybrid, std::move(VirtualWD));
  }

  /// Construct a live working directory.
  static VirtualWorkingDirectory createLive() {
    return VirtualWorkingDirectory(Live, Style::native);
  }

  /// Construct a virtual working directory that's a snapshot of the working
  /// directory state on the live filesystem. Errors only if \a
  /// sys::fs::current_paths().
  static Expected<VirtualWorkingDirectory> createVirtualSnapshot();

  /// Construct a virtual working directory.
  explicit VirtualWorkingDirectory(Style PathStyle = Style::native)
      : VirtualWorkingDirectory(Virtual, PathStyle) {}

  /// Construct a virtual working directory with existing state.
  explicit VirtualWorkingDirectory(WorkingDirectoryState VirtualWD)
      : VirtualWorkingDirectory(Virtual, std::move(VirtualWD)) {}

private:
  explicit VirtualWorkingDirectory(Mode M, WorkingDirectoryState VirtualWD)
      : M(M), VirtualWD(std::move(VirtualWD)) {}

  explicit VirtualWorkingDirectory(Mode M, Style PathStyle = Style::native)
      : M(M), VirtualWD(PathStyle) {}

  Mode M;
  sys::path::WorkingDirectoryState VirtualWD;
};

} // end namespace vfs
} // end namespace llvm

#endif // LLVM_SUPPORT_VIRTUALWORKINGDIRECTORY_H
