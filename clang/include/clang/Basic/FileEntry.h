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

/// A reference to a \c FileEntry that includes the name of the file as it was
/// accessed by the FileManager's client.
class FileEntryRef {
public:
  StringRef getName() const { return ME->first(); }
  const FileEntry &getFileEntry() const {
    return *ME->second->V.get<FileEntry *>();
  }

  inline bool isValid() const;
  inline off_t getSize() const;
  inline unsigned getUID() const;
  inline const llvm::sys::fs::UniqueID &getUniqueID() const;
  inline time_t getModificationTime() const;
  inline void closeFile() const;

  /// Check if the underlying FileEntry is the same, intentially ignoring
  /// whether the file was referenced with the same spelling of the filename.
  friend bool operator==(const FileEntryRef &LHS, const FileEntryRef &RHS) {
    return &LHS.getFileEntry() == &RHS.getFileEntry();
  }
  friend bool operator==(const FileEntry *LHS, const FileEntryRef &RHS) {
    return LHS == &RHS.getFileEntry();
  }
  friend bool operator==(const FileEntryRef &LHS, const FileEntry *RHS) {
    return &LHS.getFileEntry() == RHS;
  }
  friend bool operator!=(const FileEntryRef &LHS, const FileEntryRef &RHS) {
    return !(LHS == RHS);
  }
  friend bool operator!=(const FileEntry *LHS, const FileEntryRef &RHS) {
    return !(LHS == RHS);
  }
  friend bool operator!=(const FileEntryRef &LHS, const FileEntry *RHS) {
    return !(LHS == RHS);
  }

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
  bool isSameRef(const FileEntryRef &RHS) const { return ME == RHS.ME; }

  /// Allow FileEntryRef to degrade into FileEntry* to facilitate incremental
  /// adoption.
  operator const FileEntry *() const { return &getFileEntry(); }

  FileEntryRef() = delete;
  explicit FileEntryRef(const MapEntry &ME) : ME(&ME) {
    assert(ME.second && "Expected payload");
    assert(ME.second->V && "Expected non-null");
    assert(ME.second->V.is<FileEntry *>() && "Expected FileEntry");
  }

  /// Expose the underlying MapEntry to simplify packing in a PointerIntPair or
  /// PointerUnion and allow construction in Optional.
  const clang::FileEntryRef::MapEntry &getMapEntry() const { return *ME; }

private:
  const MapEntry *ME;
};

static_assert(sizeof(FileEntryRef) == sizeof(const FileEntry *),
              "FileEntryRef must avoid size overhead");

} // end namespace clang

namespace llvm {

/// Customize Optional<FileEntryRef> to use FileEntryRef::MapEntry directly
/// in order to keep it to the size of a pointer.
///
/// Note that customizing OptionalStorage would not work, since the default
/// implementation of Optional returns references and pointers to the stored
/// value, and this customization does not store a full FileEntryRef.
template <> class Optional<clang::FileEntryRef> {
  const clang::FileEntryRef::MapEntry *ME = nullptr;

public:
  using value_type = clang::FileEntryRef;

  Optional() {}
  Optional(NoneType) {}

  Optional(clang::FileEntryRef Ref) : ME(&Ref.getMapEntry()) {}
  Optional(const Optional &O) = default;

  /// Create a new object by constructing it in place with the given arguments.
  template <typename... ArgTypes> void emplace(ArgTypes &&...Args) {
    clang::FileEntryRef Ref(std::forward<ArgTypes>(Args)...);
    *this = Ref;
  }

  static Optional create(const clang::FileEntryRef *Ref) {
    return Ref ? Optional(*Ref) : Optional();
  }

  Optional &operator=(clang::FileEntryRef Ref) {
    ME = &Ref.getMapEntry();
    return *this;
  }
  Optional &operator=(const Optional &O) = default;

  void reset() { ME = nullptr; }

  /// Type for use as a "pointer" return for the arrow operator, encapsulating
  /// a real FileEntryRef.
  class pointer {
    friend class Optional;

  public:
    const clang::FileEntryRef *operator->() const { return &Ref; }

    pointer() = delete;
    pointer(const pointer &p) = default;

  private:
    explicit pointer(clang::FileEntryRef Ref) : Ref(Ref) {}
    clang::FileEntryRef Ref;
  };

  clang::FileEntryRef getValue() const LLVM_LVALUE_FUNCTION {
    assert(ME && "Dereferencing None?");
    return clang::FileEntryRef(*ME);
  }

  explicit operator bool() const { return hasValue(); }
  bool hasValue() const { return ME; }
  pointer operator->() const { return pointer(getValue()); }
  clang::FileEntryRef operator*() const LLVM_LVALUE_FUNCTION {
    return getValue();
  }

  template <typename U>
  clang::FileEntryRef getValueOr(U &&value) const LLVM_LVALUE_FUNCTION {
    return hasValue() ? getValue() : std::forward<U>(value);
  }

  /// Apply a function to the value if present; otherwise return None.
  template <class Function>
  auto map(const Function &F) const LLVM_LVALUE_FUNCTION
      -> Optional<decltype(F(getValue()))> {
    if (*this)
      return F(getValue());
    return None;
  }

  /// Allow Optional<FileEntryRef> to degrade into FileEntry* to facilitate
  /// incremental adoption of FileEntryRef. Once FileEntry::getName, remaining
  /// uses of this should be cleaned up and this can be removed as well.
  operator const clang::FileEntry *() const {
    return hasValue() ? &getValue().getFileEntry() : nullptr;
  }

  /// Allow construction from the underlying pointer type to simplify
  /// constructing from a PointerIntPair or PointerUnion.
  explicit Optional(const clang::FileEntryRef::MapEntry *ME) : ME(ME) {}

  /// Expose the underlying pointer type to simplify packing in a
  /// PointerIntPair or PointerUnion.
  const clang::FileEntryRef::MapEntry *getMapEntry() const { return ME; }
};

static_assert(sizeof(Optional<clang::FileEntryRef>) ==
                  sizeof(clang::FileEntryRef),
              "Optional<FileEntryRef> must avoid size overhead");

} // end namespace llvm

namespace clang {

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
  Optional<FileEntryRef> LastRef;

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
