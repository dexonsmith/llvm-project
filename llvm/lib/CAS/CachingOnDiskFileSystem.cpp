//===- CachingOnDiskFileSystem.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/CachingOnDiskFileSystem.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/CAS/CASDB.h"
#include "llvm/CAS/HashMappedTrie.h"
#include "llvm/CAS/HierarchicalTreeBuilder.h"
#include "llvm/Support/AlignOf.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/SHA1.h"
#include <mutex>

// FIXME: Put the functions we need in FileSystem.h.
#include <fcntl.h>
#include <unistd.h>

using namespace llvm;
using namespace llvm::cas;

void ThreadSafeFileSystem::anchor() {}
void CachingOnDiskFileSystem::anchor() {}

namespace {

class CachingOnDiskFileSystemImpl final : public CachingOnDiskFileSystem {
  struct WorkingDirectoryType {
    FileSystemCache::DirectoryEntry *Entry;

    /// Mimics shell behaviour on directory changes. Not necessarily the same
    /// as \c Entry->getTreePath().
    std::string Path;
  };

  class VFSFile; // Return type for vfs::FileSystem::openFileForRead().
  class TreeBuilder;

public:
  using File = FileSystemCache::File;
  using Symlink = FileSystemCache::Symlink;
  using Directory = FileSystemCache::Directory;
  using DirectoryEntry = FileSystemCache::DirectoryEntry;

  Expected<const vfs::CachedDirectoryEntry *>
  getDirectoryEntry(const Twine &Path, bool FollowSymlinks) const override;

  /// Look up a directory entry in the CAS, navigating trees and resolving
  /// symlinks in the parent path. If \p FollowSymlinks is true, also follows
  /// symlinks in the filename.
  ///
  /// If \p TrackNonRealPathEntries is given, the links in the symlink chain and
  /// the final path are passed to it as the search progresses.
  Expected<DirectoryEntry *> lookupPath(
      StringRef Path, bool FollowSymlinks = true, bool LookupOnDisk = true,
      function_ref<void(DirectoryEntry &)> TrackNonRealPathEntries = nullptr);

  DirectoryEntry *makeDirectory(DirectoryEntry &Parent, StringRef TreePath);

  Expected<DirectoryEntry *> makeSymlink(DirectoryEntry &Parent,
                                         const std::string &TreePath);

  Expected<DirectoryEntry *> makeFile(DirectoryEntry &Parent,
                                      StringRef TreePath, int BorrowedFD,
                                      sys::fs::file_status Status);

  /// Preload the real path for \p Remaining, relative to \p From.
  Error preloadRealPath(DirectoryEntry &From, StringRef Remaining);

  /// Look up a name on disk inside \p From.
  Expected<DirectoryEntry *> lookupOnDiskFrom(DirectoryEntry &Parent,
                                              StringRef Name);

  ErrorOr<vfs::Status> statusAndFileID(const Twine &Path,
                                       Optional<CASID> &FileID) final;
  Optional<CASID> getFileCASID(const Twine &Path) final;
  ErrorOr<vfs::Status> status(const Twine &Path) final;
  ErrorOr<std::unique_ptr<vfs::File>> openFileForRead(const Twine &Path) final;
  vfs::directory_iterator dir_begin(const Twine &Dir,
                                    std::error_code &EC) final {
    auto IterOr = getDirectoryIterator(Dir);
    if (IterOr)
      return *IterOr;
    EC = IterOr.getError();
    return vfs::directory_iterator();
  }

  std::error_code getRealPath(const Twine &Path,
                              SmallVectorImpl<char> &Output) const final;
  ErrorOr<vfs::directory_iterator> getDirectoryIterator(const Twine &Dir);

  std::error_code setCurrentWorkingDirectory(const Twine &Path) final;

  ErrorOr<std::string> getCurrentWorkingDirectory() const final {
    return WorkingDirectory.Path;
  }

  static StringRef canonicalizeWorkingDirectory(const Twine &Path,
                                                StringRef WorkingDirectory,
                                                SmallVectorImpl<char> &Storage);

  void trackNewAccesses() final;
  Expected<TreeRef> createTreeFromNewAccesses(
      llvm::function_ref<StringRef(const vfs::CachedDirectoryEntry &)>
          RemapPath) final;
  Expected<TreeRef> createTreeFromAllAccesses() final;
  std::unique_ptr<CachingOnDiskFileSystem::TreeBuilder>
  createTreeBuilder() final;
  Error pushCachedPath(const Twine &Path, TreeBuilder &State);

  IntrusiveRefCntPtr<CachingOnDiskFileSystem> createProxyFS() final {
    return makeIntrusiveRefCnt<CachingOnDiskFileSystemImpl>(*this);
  }

  CachingOnDiskFileSystemImpl(std::shared_ptr<CASDB> DB)
      : CachingOnDiskFileSystem(std::move(DB)) {
    initializeWorkingDirectory();
  }
  CachingOnDiskFileSystemImpl(CASDB &DB) : CachingOnDiskFileSystem(DB) {
    initializeWorkingDirectory();
  }

  CachingOnDiskFileSystemImpl(const CachingOnDiskFileSystemImpl &Proxy)
      : CachingOnDiskFileSystem(Proxy), Cache(Proxy.Cache),
        WorkingDirectory(Proxy.WorkingDirectory) {}

private:
  void initializeWorkingDirectory();

  // Cached stats. Useful for tracking everything that has been stat'ed.
  Optional<DenseSet<const DirectoryEntry *>> TrackedAccesses;
  std::mutex TrackedAccessesMutex;

  IntrusiveRefCntPtr<FileSystemCache> Cache;
  WorkingDirectoryType WorkingDirectory;
};

} // namespace

CachingOnDiskFileSystem::CachingOnDiskFileSystem(std::shared_ptr<CASDB> DB)
    : DB(*DB), OwnedDB(std::move(DB)) {}

CachingOnDiskFileSystem::CachingOnDiskFileSystem(CASDB &DB) : DB(DB) {}

class CachingOnDiskFileSystemImpl::VFSFile : public vfs::File {
public:
  ErrorOr<vfs::Status> status() final { return Entry->getStatus(Name); }

  ErrorOr<std::string> getName() final { return Name; }

  /// Get the contents of the file as a \p MemoryBuffer.
  ErrorOr<std::unique_ptr<MemoryBuffer>> getBuffer(const Twine &RequestedName,
                                                   int64_t, bool, bool) final {
    SmallString<256> Storage;
    if (Expected<BlobRef> ExpectedBlob = DB.getBlob(*Entry->getID()))
      return MemoryBuffer::getMemBuffer(**ExpectedBlob,
                                        RequestedName.toStringRef(Storage));
    else
      return errorToErrorCode(ExpectedBlob.takeError());
  }

  /// Closes the file.
  std::error_code close() final { return std::error_code(); }

  VFSFile() = delete;
  explicit VFSFile(CASDB &DB, DirectoryEntry &Entry, StringRef Name)
      : DB(DB), Entry(&Entry), Name(Name.str()) {}

private:
  CASDB &DB;
  DirectoryEntry *Entry;
  std::string Name;
};

void CachingOnDiskFileSystemImpl::initializeWorkingDirectory() {
  Cache = makeIntrusiveRefCnt<FileSystemCache>();

  // Start with root, and then initialize the current working directory to
  // match process state, ignoring errors if there's a problem.
  WorkingDirectory.Entry = &Cache->getRoot();
  WorkingDirectory.Path = WorkingDirectory.Entry->getTreePath().str();

  SmallString<128> CWD;
  if (std::error_code EC = llvm::sys::fs::current_path(CWD)) {
    (void)EC;
    return;
  }
  std::error_code EC = setCurrentWorkingDirectory(CWD);
  (void)EC;
}

std::error_code
CachingOnDiskFileSystemImpl::setCurrentWorkingDirectory(const Twine &Path) {
  SmallString<128> Storage;
  StringRef CanonicalPath =
      canonicalizeWorkingDirectory(Path, WorkingDirectory.Path, Storage);

  // Read and cache all the symlinks in the path by looking it up. Return any
  // error encountered.
  Expected<DirectoryEntry *> ExpectedEntry = lookupPath(CanonicalPath);
  if (!ExpectedEntry)
    return errorToErrorCode(ExpectedEntry.takeError());

  WorkingDirectory.Path = CanonicalPath.str();
  WorkingDirectory.Entry = *ExpectedEntry;
  return std::error_code();
}

StringRef CachingOnDiskFileSystemImpl::canonicalizeWorkingDirectory(
    const Twine &Path, StringRef WorkingDirectory,
    SmallVectorImpl<char> &Storage) {
  // Not portable.
  assert(WorkingDirectory.startswith("/"));
  Path.toVector(Storage);
  if (Storage.empty())
    return WorkingDirectory;

  if (Storage[0] != '/') {
    SmallString<128> Prefix = StringRef(WorkingDirectory);
    Prefix.push_back('/');
    Storage.insert(Storage.begin(), Prefix.begin(), Prefix.end());
  }

  // Remove ".." components based on working directory string, not based on
  // real path. This matches shell behaviour.
  sys::path::remove_dots(Storage, /*remove_dot_dot=*/true,
                         sys::path::Style::posix);

  // Remove double slashes.
  int W = 0;
  bool WasSlash = false;
  for (int R = 0, E = Storage.size(); R != E; ++R) {
    bool IsSlash = Storage[R] == '/';
    if (IsSlash && WasSlash)
      continue;
    WasSlash = IsSlash;
    Storage[W++] = Storage[R];
  }
  Storage.resize(W);

  // Remove final slash.
  if (Storage.size() > 1 && Storage.back() == '/')
    Storage.pop_back();

  return StringRef(Storage.begin(), Storage.size());
}

FileSystemCache::DirectoryEntry *
CachingOnDiskFileSystemImpl::makeDirectory(DirectoryEntry &Parent,
                                           StringRef TreePath) {
  return &Cache->makeDirectory(Parent, TreePath);
}

Expected<FileSystemCache::DirectoryEntry *>
CachingOnDiskFileSystemImpl::makeSymlink(DirectoryEntry &Parent,
                                         const std::string &TreePath) {
  char TargetBuffer[PATH_MAX] = {0};
  int TargetLength =
      ::readlink(TreePath.c_str(), TargetBuffer, sizeof(TargetBuffer));
  if (TargetLength == -1)
    return errorCodeToError(std::error_code(errno, std::generic_category()));
  StringRef Target = StringRef(TargetBuffer, TargetLength);

  Expected<BlobRef> Blob = DB.createBlob(Target);
  if (!Blob)
    return Blob.takeError();

  return &Cache->makeSymlink(Parent, TreePath, *Blob, **Blob);
}

Expected<FileSystemCache::DirectoryEntry *>
CachingOnDiskFileSystemImpl::makeFile(DirectoryEntry &Parent,
                                      StringRef TreePath, int BorrowedFD,
                                      sys::fs::file_status Status) {
  Expected<BlobRef> ExpectedBlob =
      DB.createBlobFromOpenFile(BorrowedFD, Status);
  if (!ExpectedBlob)
    return ExpectedBlob.takeError();

  // Do not trust Status.size() in case the file is volatile.
  return &Cache->makeFile(Parent, TreePath, *ExpectedBlob,
                          ExpectedBlob->getData().size(),
                          Status.permissions() & sys::fs::perms::owner_exe);
}

Expected<FileSystemCache::DirectoryEntry *>
CachingOnDiskFileSystemImpl::lookupOnDiskFrom(DirectoryEntry &Parent,
                                              StringRef Name) {
  assert(Parent.isDirectory() && "Expected a directory");

  // Open the path on disk.
  StringRef ParentPath = Parent.getTreePath();
  std::string Path =
      (ParentPath.endswith("/") ? ParentPath + Name : ParentPath + "/" + Name)
          .str();

  // lstat is extremely slow...
  sys::fs::file_status Status;
  if (std::error_code EC = sys::fs::status(Path, Status, /*follow=*/false))
    return errorCodeToError(EC);

  if (Status.type() == sys::fs::file_type::directory_file)
    return makeDirectory(Parent, Path);

  if (Status.type() == sys::fs::file_type::symlink_file)
    return makeSymlink(Parent, Path);

  int FD;
  if (std::error_code EC =
          sys::fs::openFile(Path, FD, sys::fs::CD_OpenExisting,
                            sys::fs::FA_Read, sys::fs::OF_None))
    return errorCodeToError(EC);
  auto CloseOnExit = make_scope_exit([&FD]() { ::close(FD); });
  return makeFile(Parent, Path, FD, Status);
}

ErrorOr<vfs::Status>
CachingOnDiskFileSystemImpl::statusAndFileID(const Twine &Path,
                                             Optional<CASID> &FileID) {
  FileID = None;
  SmallString<128> Storage;
  StringRef PathRef = Path.toStringRef(Storage);

  // Lookup only returns an Error if there's a problem communicating with the
  // CAS, or there's data corruption.
  //
  // FIXME: Translate the error to a filesystem-like error to encapsulate the
  // user from CAS issues.
  Expected<DirectoryEntry *> ExpectedEntry = lookupPath(PathRef);
  if (!ExpectedEntry)
    return errorToErrorCode(ExpectedEntry.takeError());

  // Errors indicate a broken symlink.
  DirectoryEntry *Entry = *ExpectedEntry;
  ErrorOr<vfs::Status> StatusOrErr = Entry->getStatus(PathRef);
  if (!StatusOrErr)
    return StatusOrErr.getError();
  if (Entry->isFile())
    FileID = *Entry->getID();
  return StatusOrErr;
}

Optional<CASID> CachingOnDiskFileSystemImpl::getFileCASID(const Twine &Path) {
  // FIXME: This is the correct implementation, but hack it out for now to
  // focus on CASFileSystem.
  //
  // return None;
  //
  // ... or don't hack it out.

  Optional<CASID> ID;
  (void)statusAndFileID(Path, ID);
  return ID;
}

Expected<const vfs::CachedDirectoryEntry *>
CachingOnDiskFileSystemImpl::getDirectoryEntry(const Twine &Path,
                                               bool FollowSymlinks) const {
  SmallString<128> Storage;
  StringRef PathRef = Path.toStringRef(Storage);

  // It's not a const operation, but it's thread-safe.
  return const_cast<CachingOnDiskFileSystemImpl *>(this)->lookupPath(
      PathRef, FollowSymlinks);
}

std::error_code
CachingOnDiskFileSystemImpl::getRealPath(const Twine &Path,
                                         SmallVectorImpl<char> &Output) const {
  // We can get the real path, but it's not a const operation.
  const vfs::CachedDirectoryEntry *Entry = nullptr;
  if (Error E =
          getDirectoryEntry(Path, /*FollowSymlinks=*/true).moveInto(Entry))
    return errorToErrorCode(std::move(E));

  StringRef TreePath = Entry->getTreePath();
  Output.resize(TreePath.size());
  llvm::copy(TreePath, Output.begin());
  return std::error_code();
}

ErrorOr<vfs::Status> CachingOnDiskFileSystemImpl::status(const Twine &Path) {
  Optional<CASID> IgnoredID;
  return statusAndFileID(Path, IgnoredID);
}

ErrorOr<std::unique_ptr<vfs::File>>
CachingOnDiskFileSystemImpl::openFileForRead(const Twine &Path) {
  SmallString<128> Storage;
  StringRef PathRef = Path.toStringRef(Storage);

  Expected<DirectoryEntry *> ExpectedEntry = lookupPath(PathRef);
  if (!ExpectedEntry)
    return errorToErrorCode(ExpectedEntry.takeError());

  DirectoryEntry *Entry = *ExpectedEntry;
  if (!Entry->isFile())
    return std::errc::invalid_argument;

  return std::make_unique<VFSFile>(DB, *Entry, PathRef);
}

ErrorOr<vfs::directory_iterator>
CachingOnDiskFileSystemImpl::getDirectoryIterator(const Twine &Path) {
  SmallString<128> Storage;
  StringRef PathRef = Path.toStringRef(Storage);

  Expected<DirectoryEntry *> ExpectedEntry = lookupPath(PathRef);
  if (!ExpectedEntry)
    return errorToErrorCode(ExpectedEntry.takeError());

  DirectoryEntry *Entry = *ExpectedEntry;
  if (!Entry->isDirectory())
    return std::errc::not_a_directory;

  // Walk the directory on-disk to discover entries.
  std::error_code EC;
  SmallVector<std::string> Names;
  for (sys::fs::directory_iterator I(Entry->getTreePath(), EC), E;
       !EC && I != E; I.increment(EC))
    Names.emplace_back(sys::path::filename(I->path()).str());
  if (EC)
    return EC;

  // Cache all the entries.
  Directory &D = Entry->asDirectory();

  // Filter out names that we know about.
  {
    Directory::Reader R(D);
    Names.erase(
        llvm::remove_if(Names, [&D](StringRef Name) { return D.lookup(Name); }),
        Names.end());
  }

  for (StringRef Name : Names)
    if (Error E = lookupOnDiskFrom(*Entry, Name).takeError())
      return errorToErrorCode(std::move(E));

  return Cache->getCachedVFSDirIter(
      D, [this](StringRef Path) { return lookupPath(Path); },
      WorkingDirectory.Path, PathRef);
}

Error CachingOnDiskFileSystemImpl::preloadRealPath(DirectoryEntry &From,
                                                   StringRef Remaining) {
  SmallString<256> ExpectedRealPath;
  ExpectedRealPath = From.getTreePath();
  sys::path::append(ExpectedRealPath, Remaining);

  // Most paths don't exist. Start with a stat. Profiling says this is faster
  // on Darwin when running clang-scan-deps (looks like allocation traffic in
  // ::open on stat failures). This may be platform- or even
  // OS-version-dependent though.
  {
    sys::fs::file_status Status;
    if (std::error_code EC = sys::fs::status(ExpectedRealPath, Status))
      return errorCodeToError(EC);
    if (!sys::fs::exists(Status))
      return errorCodeToError(
          std::make_error_code(std::errc::no_such_file_or_directory));

    // Don't reuse Status below since there could be a race.
  }

  int FD;
  SmallString<256> RealPath;
  if (std::error_code EC = sys::fs::openFileForRead(
          ExpectedRealPath, FD, sys::fs::OF_None, &RealPath))
    return errorCodeToError(EC);
  auto CloseOnExit = make_scope_exit([&FD]() { ::close(FD); });

  FileSystemCache::LookupPathState State(Cache->getRoot(), RealPath);

  // Advance through the cached directories. Note: no need to pass through
  // TrackNonRealPathEntries because we're navigating a real path.
  StringRef ExpectedPrefix =
      StringRef(ExpectedRealPath).drop_back(Remaining.size());
  if (RealPath.startswith(ExpectedPrefix))
    State = FileSystemCache::LookupPathState(
        From, RealPath.substr(ExpectedPrefix.size()));
  else
    State = Cache->lookupRealPathPrefixFromCached(
        State, /*TrackNonRealPathEntries=*/nullptr);

  // Real path is already fully cached.
  if (State.Remaining.empty())
    return Error::success();

  // All but the last component must be directories.
  while (!State.AfterName.empty()) {
    DirectoryEntry &Entry = Cache->makeDirectory(
        *State.Entry, RealPath.substr(0, State.Name.end() - RealPath.begin()));

    // If we don't get back a directory, the disk state must have changed and
    // another thread raced. Give up on this endeavour.
    if (!Entry.isDirectory())
      return Error::success();

    State.advance(Entry);
  }

  assert(!State.Name.empty());

  // Skip all errors from here out. This is just priming the cache.
  sys::fs::file_status Status;
  if (/*std::error_code EC =*/sys::fs::status(FD, Status))
    return Error::success();

  if (Status.type() == sys::fs::file_type::directory_file) {
    (void)makeDirectory(*State.Entry, RealPath);
    return Error::success();
  }

  if (Error E = makeFile(*State.Entry, RealPath, FD, Status).takeError())
    llvm::consumeError(std::move(E));
  return Error::success();
}

Expected<FileSystemCache::DirectoryEntry *>
CachingOnDiskFileSystemImpl::lookupPath(
    StringRef Path, bool FollowSymlinks, bool LookupOnDisk,
    function_ref<void(FileSystemCache::DirectoryEntry &)>
        TrackNonRealPathEntries) {
  auto RequestDirectoryEntryHelper =
      [this](FileSystemCache::DirectoryEntry &Parent, StringRef Name) {
        return lookupOnDiskFrom(Parent, Name);
      };
  auto PreloadTreePathHelper = [this](DirectoryEntry &From,
                                      StringRef Remaining) {
    return preloadRealPath(From, Remaining);
  };

  bool IsTrackingStats = false;
  {
    std::lock_guard<std::mutex> Lock(TrackedAccessesMutex);
    IsTrackingStats = TrackedAccesses ? true : false;
  }

  auto TrackNonRealPathEntriesHelper =
      [this, TrackNonRealPathEntries](FileSystemCache::DirectoryEntry &Entry) {
        if (TrackNonRealPathEntries)
          TrackNonRealPathEntries(Entry);
        std::lock_guard<std::mutex> Lock(TrackedAccessesMutex);
        TrackedAccesses->insert(&Entry);
      };

  // Careful of use-after-scope.
  FileSystemCache::RequestDirectoryEntryType RequestDirectoryEntry = nullptr;
  FileSystemCache::PreloadTreePathType PreloadTreePath = nullptr;
  if (LookupOnDisk) {
    RequestDirectoryEntry = RequestDirectoryEntryHelper;
    PreloadTreePath = PreloadTreePathHelper;
  }
  if (TrackedAccesses)
    TrackNonRealPathEntries = TrackNonRealPathEntriesHelper;

  Expected<DirectoryEntry *> ExpectedEntry =
      Cache->lookupPath(Path, *WorkingDirectory.Entry, RequestDirectoryEntry,
                        /*RequestSymlinkTarget=*/nullptr, PreloadTreePath,
                        FollowSymlinks, TrackNonRealPathEntries);
  if (IsTrackingStats && ExpectedEntry && *ExpectedEntry) {
    std::lock_guard<std::mutex> Lock(TrackedAccessesMutex);
    TrackedAccesses->insert(*ExpectedEntry);
  }
  return ExpectedEntry;
}

static TreeEntry::EntryKind
getTreeEntryKind(const FileSystemCache::DirectoryEntry &Entry) {
  switch (Entry.getKind()) {
  case FileSystemCache::DirectoryEntry::Directory:
    return TreeEntry::Tree;
  case FileSystemCache::DirectoryEntry::Symlink:
    return TreeEntry::Symlink;
  case FileSystemCache::DirectoryEntry::Regular:
    return TreeEntry::Regular;
  case FileSystemCache::DirectoryEntry::Executable:
    return TreeEntry::Executable;
  }
}

/// Push an entry to the builder, doing nothing (but returning false) for
/// directories.
static void pushEntryToBuilder(HierarchicalTreeBuilder &Builder,
                               const FileSystemCache::DirectoryEntry &Entry) {
  assert(!Entry.isDirectory());

  if (Entry.isSymlink()) {
    Builder.push(*Entry.getID(), TreeEntry::Symlink, Entry.getTreePath());
    return;
  }

  Builder.push(*Entry.getID(), getTreeEntryKind(Entry), Entry.getTreePath());
}

void CachingOnDiskFileSystemImpl::trackNewAccesses() {
  std::lock_guard<std::mutex> Lock(TrackedAccessesMutex);
  if (TrackedAccesses)
    return;
  TrackedAccesses.emplace();
  TrackedAccesses->reserve(128); // Seed with a bit of runway.
}

Expected<TreeRef> CachingOnDiskFileSystemImpl::createTreeFromNewAccesses(
    llvm::function_ref<StringRef(const vfs::CachedDirectoryEntry &)>
        RemapPath) {
  Optional<DenseSet<const DirectoryEntry *>> MaybeTrackedAccesses;
  {
    std::lock_guard<std::mutex> Lock(TrackedAccessesMutex);
    MaybeTrackedAccesses = std::move(TrackedAccesses);
  }

  if (!MaybeTrackedAccesses || MaybeTrackedAccesses->empty())
    return DB.createTree();

  HierarchicalTreeBuilder Builder;
  for (const DirectoryEntry *Entry : *MaybeTrackedAccesses) {
    StringRef Path = RemapPath ? RemapPath(*Entry) : Entry->getTreePath();

    // FIXME: If Entry is a symbol link, the spelling of its target should be
    // remapped.
    if (Entry->isDirectory())
      Builder.pushDirectory(Path);
    else
      Builder.push(*Entry->getID(), getTreeEntryKind(*Entry), Path);
  }

  return Builder.create(DB);
}

Expected<TreeRef> CachingOnDiskFileSystemImpl::createTreeFromAllAccesses() {
  std::unique_ptr<CachingOnDiskFileSystem::TreeBuilder> Builder =
      createTreeBuilder();

  // FIXME: Not portable; only works for posix, not windows.
  //
  // FIXME: don't know if we want this push to be recursive...
  if (Error E = Builder->push("/"))
    return std::move(E);
  return Builder->create();
}

class CachingOnDiskFileSystemImpl::TreeBuilder final
    : public CachingOnDiskFileSystem::TreeBuilder {
public:
  /// Add \p Path to hierarchical tree-in-progress.
  ///
  /// If \p Path resolves to a symlink, its target is implicitly pushed as
  /// well.
  ///
  /// If \p Path resolves to a directory, the recursive directory contents
  /// will be pushed, implicitly pushing the targets of any contained symlinks.
  ///
  /// If \p Path does not exist, an error will be returned. If \p Path's parent
  /// path exists but the filename refers to a broken symlink, that is not an
  /// error; the symlink will be added without the target.
  Error push(const Twine &Path) final;

  Expected<TreeRef> create() final { return Builder.create(FS.getCAS()); }

  // Push \p Entry directly to \a Builder, asserting that it's a symlink.
  void pushSymlink(const DirectoryEntry &Entry);

  // Push \p Entry to \a Builder if it's a file, to \a Worklist otherwise.
  void pushEntry(const DirectoryEntry &Entry);

  explicit TreeBuilder(CachingOnDiskFileSystemImpl &FS) : FS(FS) {}
  HierarchicalTreeBuilder Builder;
  CachingOnDiskFileSystemImpl &FS;

  SmallString<128> PathStorage;
  SmallVector<const DirectoryEntry *> Worklist;
  DenseSet<const DirectoryEntry *> Seen;
};

std::unique_ptr<CachingOnDiskFileSystem::TreeBuilder>
CachingOnDiskFileSystemImpl::createTreeBuilder() {
  return std::make_unique<TreeBuilder>(*this);
}

void CachingOnDiskFileSystemImpl::TreeBuilder::pushSymlink(
    const DirectoryEntry &Entry) {
  assert(Entry.isSymlink());
  if (Seen.insert(&Entry).second)
    pushEntryToBuilder(Builder, Entry);
}

void CachingOnDiskFileSystemImpl::TreeBuilder::pushEntry(
    const DirectoryEntry &Entry) {
  if (!Seen.insert(&Entry).second)
    return;
  if (Entry.isFile() || Entry.isSymlink())
    pushEntryToBuilder(Builder, Entry);
  if (!Entry.isFile())
    Worklist.push_back(&Entry);
}

Error CachingOnDiskFileSystemImpl::TreeBuilder::push(const Twine &Path) {
  PathStorage.clear();
  StringRef PathRef = Path.toStringRef(PathStorage);

  // Look for Path without following symlinks. Failure here indicates that Path
  // does not exist or has a broken symlink in its parent path. Keep track of
  // symlinks in the parent path, but don't proactively push them to the
  // builder.
  SmallVector<const DirectoryEntry *> NonRealPathEntries;
  Expected<const DirectoryEntry *> PathEntry =
      FS.lookupPath(PathRef, /*FollowSymlinks=*/false,
                    /*LookupOnDisk=*/false, [&](const DirectoryEntry &Entry) {
                      NonRealPathEntries.push_back(&Entry);
                    });
  if (!PathEntry)
    return PathEntry.takeError();

  // Finish resolving Path. If it's a symlink, recursively push its target. If
  // it's a directory, recursively push its contents. Broken symlinks here are
  // fine; we should push whatever we see.
  pushEntry(**PathEntry);

  // Push any symlinks from the parent path to the builder.
  for (const DirectoryEntry *Entry : NonRealPathEntries) {
    if (Entry->isSymlink()) {
      pushSymlink(*Entry);
      continue;
    }

    // Handle directories navigated away from with "..".
    assert(Entry->isDirectory());
    Builder.pushDirectory(Entry->getTreePath());
  }

  while (!Worklist.empty()) {
    const DirectoryEntry *Current = Worklist.pop_back_val();
    assert(!Current->isFile());
    if (Current->isSymlink()) {
      if (Expected<DirectoryEntry *> ExpectedEntry = FS.lookupPath(
              Current->getTreePath(),
              /*FollowSymlinks=*/true,
              /*LookupOnDisk=*/false, [this](const DirectoryEntry &Entry) {
                // Don't use pushSymlink() here since we
                // want the final entry too.
                pushEntry(Entry);
              })) {
        pushEntry(**ExpectedEntry);
      } else
        consumeError(ExpectedEntry.takeError());
      continue;
    }

    assert(Current->isDirectory());
    Directory &D = Current->asDirectory();
    D.forEachEntryUnsorted(
        [this](const DirectoryEntry &Entry) { pushEntry(Entry); });
  }
  return Error::success();
}

Expected<IntrusiveRefCntPtr<CachingOnDiskFileSystem>>
cas::createCachingOnDiskFileSystem(std::shared_ptr<CASDB> DB) {
  return std::make_unique<CachingOnDiskFileSystemImpl>(std::move(DB));
}

Expected<IntrusiveRefCntPtr<CachingOnDiskFileSystem>>
cas::createCachingOnDiskFileSystem(CASDB &DB) {
  return std::make_unique<CachingOnDiskFileSystemImpl>(DB);
}
