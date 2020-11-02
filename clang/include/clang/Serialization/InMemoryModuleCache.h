//===- InMemoryModuleCache.h - In-memory cache for modules ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SERIALIZATION_INMEMORYMODULECACHE_H
#define LLVM_CLANG_SERIALIZATION_INMEMORYMODULECACHE_H

#include "clang/Basic/FileEntry.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>

namespace clang {

/// In-memory cache for modules.
///
/// This is a cache for modules for use across a compilation, sharing state
/// between the CompilerInstances in an implicit modules build.  It must be
/// shared by each CompilerInstance, ASTReader, ASTWriter, and ModuleManager
/// that are coordinating.
///
/// Critically, it ensures that a single process has a consistent view of each
/// PCM.  This is used by \a CompilerInstance when building PCMs to ensure that
/// each \a ModuleManager sees the same files.
class InMemoryModuleCache : public llvm::RefCountedBase<InMemoryModuleCache> {
  struct PCM {
    /// Track whether this PCM is known to be good (either built or
    /// successfully imported by a CompilerInstance/ASTReader using this
    /// cache).
    bool IsFinal = false;

    /// Track whether this PCM is known to be bad (attempted to, but failed, to
    /// load).
    bool HasFailed = false;

    PCM() = default;
  };

  /// Cache of buffer state.
  llvm::DenseMap<const FileEntry *, PCM> PCMs;

public:
  /// Remember that a load attempt has failed.
  ///
  /// \post shouldLoadPCM() returns false unless/until finalizePCM() is called.
  void rememberFailedLoad(FileEntryRef File) {
    auto &PCM = PCMs[File];
    if (!PCM.IsFinal)
      PCM.HasFailed = true;
  }

  /// Mark a PCM as fully loaded, past the point of unloading it and building a
  /// new one ourselves.
  ///
  /// \post calls to shouldLoadPCM() and isPCMFinal() return true.
  void finalizePCM(FileEntryRef File) {
    auto &PCM = PCMs[File];
    PCM.IsFinal = true;
    PCM.HasFailed = false;
  }

  /// Check whether the PCM is final and has been shown to work.
  ///
  /// \return true iff state is Final.
  bool isPCMFinal(FileEntryRef File) const { return PCMs.lookup(File).IsFinal; }

  /// Check whether the PCM should be loaded, either because it has been
  /// finalized or we don't know anything.
  ///
  /// \post if state is Unknown, change to Tentative.
  /// \return true iff state is not ToBuild.
  bool shouldLoadPCM(FileEntryRef File) const {
    return !PCMs.lookup(File).HasFailed;
  }
};

} // end namespace clang

#endif // LLVM_CLANG_SERIALIZATION_INMEMORYMODULECACHE_H
