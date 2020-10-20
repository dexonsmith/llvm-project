//===- clang/Basic/FileEntry.h - File references ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines interfaces for clang::FileEntry and clang::FileEntryRef.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_FILEENTRY_H
#define LLVM_CLANG_BASIC_FILEENTRY_H

#include "clang/Basic/LLVM.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem/UniqueID.h"

namespace llvm {
namespace vfs {

class File;

} // namespace vfs
} // namespace llvm

namespace clang {

class DirectoryEntry;
class FileEntry;

class FileEntryRefBase {
public:
  struct MapValue;

  /// Type used in the StringMap.
  using MapEntry = llvm::StringMapEntry<llvm::ErrorOr<MapValue>>;

  /// Type stored in the StringMap.
  struct MapValue {
    /// The pointer at another MapEntry is used when the FileManager should
    /// silently forward from one name to another, which occurs in Redirecting
    /// VFSs that use external names. In that case, the \c FileEntryRef
    /// returned by the \c FileManager will have the external name, and not the
    /// name that was used to lookup the file.
    llvm::PointerUnion<FileEntry *, const MapEntry *> V;

    MapValue() = delete;
    MapValue(FileEntry &FE) : V(&FE) {}
    MapValue(MapEntry &ME) : V(&ME) {}
  };

  /// Check if RHS referenced the file in exactly the same way.
  bool isSameRef(const FileEntryRefBase &RHS) const { return ME == RHS.ME; }

  /// Check if the underlying FileEntry is the same, intentially ignoring
  /// whether the file was referenced with the same spelling of the filename.
  friend bool operator==(const FileEntryRefBase &LHS,
                         const FileEntryRefBase &RHS) {
    return LHS.getFileEntry() == RHS.getFileEntry();
  }
  friend bool operator==(const FileEntry *LHS, const FileEntryRefBase &RHS) {
    return LHS == RHS.getFileEntry();
  }
  friend bool operator==(const FileEntryRefBase &LHS, const FileEntry *RHS) {
    return LHS.getFileEntry() == RHS;
  }
  friend bool operator!=(const FileEntryRefBase &LHS,
                         const FileEntryRefBase &RHS) {
    return !(LHS == RHS);
  }
  friend bool operator!=(const FileEntry *LHS, const FileEntryRefBase &RHS) {
    return !(LHS == RHS);
  }
  friend bool operator!=(const FileEntryRefBase &LHS, const FileEntry *RHS) {
    return !(LHS == RHS);
  }

protected:
  FileEntryRefBase() = delete;
  explicit FileEntryRefBase(const MapEntry *ME) : ME(ME) {
    if (!ME)
      return;
    assert(ME->second && "Expected payload");
    assert(ME->second->V && "Expected non-null");
    assert(ME->second->V.is<FileEntry *>() && "Expected FileEntry");
  }

  Optional<StringRef> getName() const {
    return ME ? ME->first() : Optional<StringRef>();
  }

  const FileEntry *getFileEntry() const {
    return ME ? ME->second->V.get<FileEntry *>() : nullptr;
  }

  const MapEntry *ME;
};

/// A reference to a \c FileEntry that includes the name of the file as it was
/// accessed by the FileManager's client.
class FileEntryRef : public FileEntryRefBase {
  friend class MaybeFileEntryRef;

public:
  StringRef getName() const { return *FileEntryRefBase::getName(); }
  const FileEntry &getFileEntry() const {
    return *FileEntryRefBase::getFileEntry();
  }

  /// Allow FileEntryRef to degrade into FileEntry* to facilitate incremental
  /// adoption.
  operator const FileEntry *() const { return &getFileEntry(); }

  inline bool isValid() const;
  inline off_t getSize() const;
  inline unsigned getUID() const;
  inline const llvm::sys::fs::UniqueID &getUniqueID() const;
  inline time_t getModificationTime() const;
  inline void closeFile() const;

  explicit FileEntryRef(const MapEntry &ME) : FileEntryRefBase(&ME) {}
};

class MaybeFileEntryRef : public FileEntryRefBase {
public:
  using FileEntryRefBase::getFileEntry;
  using FileEntryRefBase::getName;

  /// Allow MaybeFileEntryRef to degrade into FileEntry* to facilitate
  /// incremental adoption.
  operator const FileEntry *() const { return getFileEntry(); }

  explicit operator bool() const { return ME; }

  FileEntryRef operator*() const {
    assert(ME && "Dereferencing null?");
    return FileEntryRef(*ME);
  }

  /// Use Optional<FileEntryRef> to provide storage for arrow.
  Optional<FileEntryRef> operator->() const { return *this; }

  friend bool operator==(llvm::NoneType, const MaybeFileEntryRef &RHS) {
    return !RHS;
  }
  friend bool operator==(const MaybeFileEntryRef &LHS, llvm::NoneType) {
    return !LHS;
  }
  friend bool operator!=(llvm::NoneType LHS, const MaybeFileEntryRef &RHS) {
    return !(LHS == RHS);
  }
  friend bool operator!=(const MaybeFileEntryRef &LHS, llvm::NoneType RHS) {
    return !(LHS == RHS);
  }

  /// Implicitly convert to/from Optional<FileEntryRef>.
  operator Optional<FileEntryRef>() const {
    return ME ? FileEntryRef(*ME) : Optional<FileEntryRef>();
  }
  MaybeFileEntryRef(Optional<FileEntryRef> F)
      : FileEntryRefBase(F ? F->ME : nullptr) {}

  explicit MaybeFileEntryRef(const MapEntry *ME) : FileEntryRefBase(ME) {}
  MaybeFileEntryRef(llvm::NoneType = None) : FileEntryRefBase(nullptr) {}
  MaybeFileEntryRef(FileEntryRef F) : FileEntryRefBase(F) {}
};

/// Cached information about one file (either on disk
/// or in the virtual file system).
///
/// If the 'File' member is valid, then this FileEntry has an open file
/// descriptor for the file.
class FileEntry {
  friend class FileManager;

  std::string RealPathName;   // Real path to the file; could be empty.
  off_t Size;                 // File size in bytes.
  time_t ModTime;             // Modification time of file.
  const DirectoryEntry *Dir;  // Directory file lives in.
  llvm::sys::fs::UniqueID UniqueID;
  unsigned UID;               // A unique (small) ID for the file.
  bool IsNamedPipe;
  bool IsValid;               // Is this \c FileEntry initialized and valid?

  /// The open file, if it is owned by the \p FileEntry.
  mutable std::unique_ptr<llvm::vfs::File> File;

  // First access name for this FileEntry.
  //
  // This is Optional only to allow delayed construction (FileEntryRef has no
  // default constructor). It should always have a value in practice.
  //
  // TODO: remote this once everyone that needs a name uses FileEntryRef.
  MaybeFileEntryRef LastRef;

public:
  FileEntry();
  ~FileEntry();

  FileEntry(const FileEntry &) = delete;
  FileEntry &operator=(const FileEntry &) = delete;

  StringRef getName() const { return LastRef->getName(); }
  FileEntryRef getLastRef() const { return *LastRef; }

  StringRef tryGetRealPathName() const { return RealPathName; }
  bool isValid() const { return IsValid; }
  off_t getSize() const { return Size; }
  unsigned getUID() const { return UID; }
  const llvm::sys::fs::UniqueID &getUniqueID() const { return UniqueID; }
  time_t getModificationTime() const { return ModTime; }

  /// Return the directory the file lives in.
  const DirectoryEntry *getDir() const { return Dir; }

  bool operator<(const FileEntry &RHS) const { return UniqueID < RHS.UniqueID; }

  /// Check whether the file is a named pipe (and thus can't be opened by
  /// the native FileManager methods).
  bool isNamedPipe() const { return IsNamedPipe; }

  void closeFile() const;
};

bool FileEntryRef::isValid() const { return getFileEntry().isValid(); }

off_t FileEntryRef::getSize() const { return getFileEntry().getSize(); }

unsigned FileEntryRef::getUID() const { return getFileEntry().getUID(); }

const llvm::sys::fs::UniqueID &FileEntryRef::getUniqueID() const {
  return getFileEntry().getUniqueID();
}

time_t FileEntryRef::getModificationTime() const {
  return getFileEntry().getModificationTime();
}

void FileEntryRef::closeFile() const { getFileEntry().closeFile(); }

} // end namespace clang

#endif // LLVM_CLANG_BASIC_FILEENTRY_H
