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

class ProvidedModule {
public:
  /// True unless this module needs to be "checked" for compatability /
  /// configuration; if it's not compatible, should move on to another
  /// provider.
  ///
  /// FIXME: Or, should the provider only return if it's valid? How does this
  /// interact with the ASTReader?
  bool isPrecise() const;

  StringRef getName() const; // module name.
  StringRef getPCM() const; // memory buffer data.
};

class ModuleCache;

class ModuleProvider {
public:
  virtual ~ModuleProvider() = default;

  // 
  virtual Optional<ProvidedModule> getByName(StringRef Name) = 0;
};

/// If the module is already in the ModuleManager, need to use that one...
///
/// FIXME: does this make sense? Or is this the wrong layering?
class ModuleManagerProvider : public ModuleProvider {
public:
};

/// -fmodule-file=<pcm>
///
/// Note: these need to be loaded into the module manager to determine what
/// modules they hold.
class ModuleFileProvider : public ModuleProvider {
public:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS;
  SmallVector<std::string> PCMs;

  // FIXME: Is this necessary at all? Or does command-line processing fill this in?
};

/// -fprebuild-module-path
class PrebuiltModuleProvider : public ModuleProvider {
public:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS;
  SmallVector<std::string> PCMs;

  // FIXME: Is this necessary at all? Or does command-line processing fill this in?
};

/// -fmodule-file=<name>=<pcm>
///
/// Note: these are loaded on-demand. They override modules referenced by other
/// PCM files.
class NamedModuleFileProvider : public ModuleProvider {
public:

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS;
  StringMap<std::string> PCMPathsByModuleName;
};

class ModuleBuilder {
public:
  virtual Expected<std::unique_ptr<MemoryBuffer>>
  buildModule(std::shared_ptr<CompilerInvocation> Invocation) = 0;

  /// Optionally, write modules out to disk.
  ///
  /// FIXME: Should this be in a subclass? Should this be an OutputBackend?
  Optional<std::string> ModulesCachePath;
};

/// Build modules on-demand.
class BuildModuleOnDemandProvider : public ModuleProvider {
public:
  /// Base compiler invocation, to be augmented.
  CompilerInvocation BaseInvocation;

  /// Virtualized builder.
  std::unique_ptr<ModuleBuilder> Builder;
};

/// Build modules on-demand and store them on-disk.
class OnDiskCacheModuleProvider : public BuildModuleOnDemandProvider {
public:
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
class SharedContextModuleBuilder : public ModuleBuilder {
public:
  // Reused across each CompilerInstance.
  IntrusiveRefCntPtr<FileManager> FM;
  std::shared_ptr<PCHContainerOperations> PCHOps;

  // Stack of modules being built, for diagnostics.
  ModuleBuildStack Stack;
};

/// Build modules in an isolated context.
class IsolatedContextModuleBuilder : public ModuleBuilder {
public:
};


class ModuleCache {
public:

private:
  struct StoredModule {
  };
  StringMap<StringRef, std::unique_ptr<llvm::MemoryBuffer>> ModulesByName;
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
