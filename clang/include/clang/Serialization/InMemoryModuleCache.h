//===- InMemoryModuleCache.h - In-memory cache for modules ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SERIALIZATION_INMEMORYMODULECACHE_H
#define LLVM_CLANG_SERIALIZATION_INMEMORYMODULECACHE_H

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>

namespace clang {

class ModuleCache;

class PCMProvider {
public:
  virtual ~PCMProvider() = default;

  virtual std::unique_ptr<MemoryBuffer> getPCM(StringRef ModuleName) = 0;
};

/// -fmodule-file=<pcm>
///
/// Note: these need to be loaded into the module manager to determine what
/// modules they hold.
class ModuleFilePCMProvider : public PCMProvider {
public:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS;
  SmallVector<std::string> ModuleFiles;

  // FIXME: Is this necessary at all? Or does command-line processing fill this in?
  std::unique_ptr<MemoryBuffer> getPCM(StringRef ModuleName) override;
};

/// -fprebuilt-module-path and -fmodule-file=<name>=<pcm>
class PrebuiltModulePCMProvider : public PCMProvider {
public:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS;

  SmallVector<std::string> PrebuiltModulePaths;
  StringMap<std::string> PrebuiltModuleFiles;

  std::unique_ptr<MemoryBuffer> getPCM(StringRef ModuleName) override;
};

class PCMBuilder {
public:
  /// Build the PCM corresponding to \p Invocation and return a buffer
  /// containing it. Optionally, write it to disk at \p Output.
  virtual Expected<std::unique_ptr<MemoryBuffer>>
  buildModule(std::shared_ptr<CompilerInvocation> Invocation,
              std::unique_ptr<OutputFile> Output = nullptr) = 0;
};

class OnDemandPCM

/// Build modules on-demand.
class OnDemandPCMProvider : public PCMProvider {
public:
  std::unique_ptr<MemoryBuffer> getPCM(StringRef ModuleName) final;

  virtual std::shared_ptr<CompilerInvocation>  getPCM(StringRef ModuleName) final;

  /// Base compiler invocation, to be augmented.
  CompilerInvocation BaseInvocation;

  /// Virtualized builder.
  std::unique_ptr<PCMBuilder> Builder;
};

/// Build modules on-demand and store them on-disk.
class OnDiskCachePCMProvider : public OnDemandPCMProvider {
public:
  std::unique_ptr<MemoryBuffer> getPCM(StringRef ModuleName) override;

  /// FIXME: Should this really use the VFS? For the implicit modules cache, it
  /// might make more sense to access the disk directly... it's already
  /// bypassing stat-caching and using a lock manager...
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS;

  // FIXME: Back reference might be needed for loading other modules to check
  // if this module is "valid"... or maybe the PCM is always returned, and the
  // caller is responsible for checking that.
  ModuleCache &OwningModuleCache;

  std::string ModulesCachePath;
};

/// Build modules in a shared context.
class SharedContextModuleBuilder : public PCMBuilder {
public:
  // Reused across each CompilerInstance.
  IntrusiveRefCntPtr<FileManager> FM;
  std::shared_ptr<PCHContainerOperations> PCHOps;

  // Stack of modules being built, for diagnostics.
  ModuleBuildStack Stack;
};

/// Build modules in an isolated context.
class IsolatedContextModuleBuilder : public PCMBuilder {
public:
};

/// PCMCache is virtualized through a number of mechanisms.
///
/// PCMCache directly uses a sequence (stack or queue?) of PCMProviders, which
/// given a module name can/will find a PCM to deserialize.
///
/// OnDemandPCMProvider, if used, has options to configure how to build
/// modules:
/// - Shared vs. isolated context
/// - Fuzzy vs. exact
///
/// 1. Providing PCMs, which may be:
///   - Already built in a file / path.
///   - Buildable.
/// 2. Building PCMs, potentially using a shared context, or otherwise.
/// 3. 
class PCMCache {
public:

private:
  /// PCMs that have been loaded, by module name.
  StringMap<std::unique_ptr<llvm::MemoryBuffer>> LoadedPCMs;

  /// PCMs that have not (yet) been loaded, by module name. These could be
  /// rejected by the compiler.
  StringMap<std::unique_ptr<llvm::MemoryBuffer>> UnloadedPCMs;
};

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
    std::unique_ptr<llvm::MemoryBuffer> Buffer;

    /// Track whether this PCM is known to be good (either built or
    /// successfully imported by a CompilerInstance/ASTReader using this
    /// cache).
    bool IsFinal = false;

    PCM() = default;
    PCM(std::unique_ptr<llvm::MemoryBuffer> Buffer)
        : Buffer(std::move(Buffer)) {}
  };

  /// Cache of buffers.
  llvm::StringMap<PCM> PCMs;

public:
  /// There are four states for a PCM.  It must monotonically increase.
  ///
  ///  1. Unknown: the PCM has neither been read from disk nor built.
  ///  2. Tentative: the PCM has been read from disk but not yet imported or
  ///     built.  It might work.
  ///  3. ToBuild: the PCM read from disk did not work but a new one has not
  ///     been built yet.
  ///  4. Final: indicating that the current PCM was either built in this
  ///     process or has been successfully imported.
  enum State { Unknown, Tentative, ToBuild, Final };

  /// Get the state of the PCM.
  State getPCMState(llvm::StringRef Filename) const;

  /// Store the PCM under the Filename.
  ///
  /// \pre state is Unknown
  /// \post state is Tentative
  /// \return a reference to the buffer as a convenience.
  llvm::MemoryBuffer &addPCM(llvm::StringRef Filename,
                             std::unique_ptr<llvm::MemoryBuffer> Buffer);

  /// Store a just-built PCM under the Filename.
  ///
  /// \pre state is Unknown or ToBuild.
  /// \pre state is not Tentative.
  /// \return a reference to the buffer as a convenience.
  llvm::MemoryBuffer &addBuiltPCM(llvm::StringRef Filename,
                                  std::unique_ptr<llvm::MemoryBuffer> Buffer);

  /// Try to remove a buffer from the cache.  No effect if state is Final.
  ///
  /// \pre state is Tentative/Final.
  /// \post Tentative => ToBuild or Final => Final.
  /// \return false on success, i.e. if Tentative => ToBuild.
  bool tryToDropPCM(llvm::StringRef Filename);

  /// Mark a PCM as final.
  ///
  /// \pre state is Tentative or Final.
  /// \post state is Final.
  void finalizePCM(llvm::StringRef Filename);

  /// Get a pointer to the pCM if it exists; else nullptr.
  llvm::MemoryBuffer *lookupPCM(llvm::StringRef Filename) const;

  /// Check whether the PCM is final and has been shown to work.
  ///
  /// \return true iff state is Final.
  bool isPCMFinal(llvm::StringRef Filename) const;

  /// Check whether the PCM is waiting to be built.
  ///
  /// \return true iff state is ToBuild.
  bool shouldBuildPCM(llvm::StringRef Filename) const;
};

} // end namespace clang

#endif // LLVM_CLANG_SERIALIZATION_INMEMORYMODULECACHE_H
