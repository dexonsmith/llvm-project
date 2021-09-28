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

/// A virtual working directory, with support for tracking the live filesystem.
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
class VirtualWorkingDirectory {
public:
  /// How to initialize the working directory state in \a
  /// setCurrentWorkingDirectory() if there's no virtual state.
  enum InitializeWorkingDirectory {
    /// Error.
    IWD_Error,
    /// Don't initialize virtual state. Call \a sys::fs::set_current_path().
    IWD_Live,
    /// Construct an empty virtual working directory.
    IWD_Empty,
    /// Construct a virtual working directory from \a
    /// sys::fs::current_paths().
    IWD_Snapshot,
  };
  /// How to guess an unknown working directory.
  enum AssumeWorkingDirectory {
    /// Error.
    AWD_Error,
    /// Assume path is relative to the root.
    AWD_Root,
    /// Defer to the live filesystem and call \a sys::fs::make_absolute().
    AWD_Live,
  };
  /// How to clean the working directory path when setting it.
  enum CanonicalizeWorkingDirectory {
    /// Leave it in raw form.
    ///
    /// \code "/a/b/.//c/../" => "/a/b/.//c/../" /endcode
    CWD_Raw,
    /// Canonicalize, but keep ".." components. This matches the semantics of
    /// the POSIX \a ::chdir() system call.
    ///
    /// \code "/a/b/.//c/../" => "/a/b/c/.." /endcode
    CWD_KeepDotDot,
    /// Canonicalize, removing ".." components. This matches POSIX shell
    /// behaviour when calling \c cd on the commandline.
    ///
    /// \code "/a/b/.//c/../" => "/a/b" /endcode
    CWD_RemoveDotDot,
  };

  struct Settings {
    InitializeWorkingDirectory InitOnSet = IWD_Error;
    struct {
      AssumeWorkingDirectory Virtual = AWD_Error;
      AssumeWorkingDirectory NoVirtual = AWD_Error;
    } AssumeForUnknown;
    CanonicalizeWorkingDirectory CanonicalizeOnSet = CWD_Raw;

    bool operator==(const Settings &RHS) const {
      return InitOnSet == RHS.InitOnSet &&
             AssumeForUnknown.Virtual == RHS.AssumeForUnknown.Virtual &&
             AssumeForUnknown.NoVirtual == RHS.AssumeForUnknown.NoVirtual &&
             CanonicalizeOnSet == RHS.CanonicalizeOnSet;
    }
    bool operator!=(const Settings &RHS) const { return !operator==(RHS); }

    constexpr Settings &setCanonicalizeKeepDotDot() {
      CanonicalizeOnSet = CWD_KeepDotDot;
      return *this;
    }
    constexpr Settings &setCanonicalizeRemoveDotDot() {
      CanonicalizeOnSet = CWD_RemoveDotDot;
      return *this;
    }

    constexpr static Settings getError() { return Settings(); }

    constexpr static Settings getVirtual() {
      Settings S;
      S.InitOnSet = IWD_Empty;
      return S;
    }

    constexpr static Settings getInitLive() {
      Settings S;
      S.InitOnSet = IWD_Live;
      S.AssumeForUnknown.NoVirtual = AWD_Live;
      return S;
    }

  private:
    constexpr Settings() {}
  };

  Settings &getSettings() { return S; }
  const Settings &getSettings() const { return S; }

  /// Make \p Path absolute.
  ///
  /// If \a hasVirtualState(), calls \a
  /// sys::path::WorkingDirectoryState::makeAbsolute(), handling an unknown
  /// working directory depending on \a
  /// Settings::AssumeForUnknown::Virtual.
  ///
  /// Else, may call \a sys::fs::make_absolute(), depending on
  /// Settings::AssumeForUnknown::NoVirtual.
  ///
  /// Thread-safe.
  Error makeAbsolute(SmallVectorImpl<char> &Path) const;

  /// Set the current working directory to \p Path.
  ///
  /// If \a hasVirtualState(), it will make \p Path absolute using \a
  /// makeAbsolute(), canonicalize it depending on \a
  /// Settings::CanonicalizeOnSet, and call \a
  /// sys::path::WorkingDirectoryState::setCurrentWorkingDirectory().
  ///
  /// Else, errors, calls \a sys::fs::set_current_path(), or initializes
  /// virtual state, depending on \a Settings::InitOnSet.
  Error setCurrentWorkingDirectory(const Twine &Path);

  /// Check if there is virtual state.
  bool hasVirtualState() const { return bool(CWD); }

  sys::path::WorkingDirectoryState &getVirtualState() { return *CWD; }
  const sys::path::WorkingDirectoryState &getVirtualState() const {
    return *CWD;
  }

  /// Drop the virtual state.
  void dropVirtualState() { CWD = None; }

  /// Initialize empty working directories using \p PathStyle. Errors on
  /// relative paths.
  void initializeVirtualState(
      sys::path::Style PathStyle = sys::path::Style::native) {
    CWD.emplace(PathStyle);
  }

  /// Initialize working directories from live snapshot of filesystem. The \a
  /// sys::path::Style is always sys::path::Style::native.
  Error initializeVirtualStateFromSnapshot();

  VirtualWorkingDirectory() = default;
  explicit VirtualWorkingDirectory(Settings S) : S(S) {}
  explicit VirtualWorkingDirectory(const sys::path::WorkingDirectoryState &CWD,
                                   Settings S = Settings::getVirtual())
      : S(S), CWD(std::move(CWD)) {}
  explicit VirtualWorkingDirectory(sys::path::WorkingDirectoryState &&CWD,
                                   Settings S = Settings::getVirtual())
      : S(S), CWD(std::move(CWD)) {}

private:
  Settings S = Settings::getVirtual();
  Optional<sys::path::WorkingDirectoryState> CWD;
};

} // end namespace vfs
} // end namespace llvm

#endif // LLVM_SUPPORT_VIRTUALWORKINGDIRECTORY_H
