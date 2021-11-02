//===- llvm/CAS/CASDB.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CAS_CASDB_H
#define LLVM_CAS_CASDB_H

#include "llvm/Support/AlignOf.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CAS/CASID.h"
#include "llvm/CAS/TreeEntry.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h" // FIXME: Split out sys::fs::file_status.
#include "llvm/Support/MemoryBuffer.h"
#include <cstddef>

namespace llvm {
namespace cas {

/// Kind of CAS object.
enum class ObjectKind {
  Blob, /// Data, with no references.
  Tree, /// Filesystem-style tree, with named references and entry types.
  Node, /// Abstract hierarchical node, with data and references.
};

class ObjectIDRef {
public:
  ArrayRef<uint8_t> getHash() const { return Hash; }

private:
  ArrayRef<uint8_t> Hash;
};

class ObjectID {
public:
  ArrayRef<uint8_t> getHash() const;

  operator ObjectIDRef() const;

private:
  union {
    uint8_t Hash[sizeof(void *)];
    const uint8_t *Mem;
  };
  uint8_t HashRest[20 - sizeof(void *)];
  bool OwnsMem = true;
  uint16_t Size = 0;
};
static_assert(alignof(ObjectID) == alignof(void *), "");
static_assert(sizeof(ObjectID) == 24, "");

class FlatObjectIDArrayRef {
public:
  class iterator;

  size_t size() const { return Hashes.size() / HashSize; }
  ObjectIDRef operator[](size_t I) const;

  FlatObjectIDArrayRef(size_t HashSize, ArrayRef<uint8_t> Hashes);

private:
  size_t HashSize;
  ArrayRef<uint8_t> Hashes;
};

class Object {
  virtual void anchor();

public:
  virtual ~Object() = default;

  ObjectIDRef getID() const;
  ObjectKind getKind() const { return Kind; }
  size_t getHashSize() const { return HashSize; }

protected:
  void setSubclassData(uint8_t Data) { SubclassData = Data; }
  uint8_t getSubclassData() const { return SubclassData; }

  void updateIDRef(ObjectIDRef NewID) {
    assert(getID() == NewID && "Expected same ID, just different address");
    Hash = NewID.getHash().begin();
  }

  Object(ObjectKind Kind, ObjectIDRef ID)
      : Kind(Kind), HashSize(ID.getHash().size()), Hash(ID.getHash().begin()) {}

private:
  ObjectKind Kind;
  uint8_t SubclassData = 0;
  uint16_t HashSize;
  const uint8_t *Hash;
};

class Blob : public Object {
  void anchor() override;

public:
  virtual StringRef getData() const = 0;
  bool isNullTerminated() const { return Object::getSubclassData(); }

protected:
  void setSubclassData(uint8_t Data) = delete;
  uint8_t getSubclassData() const = delete;

  Blob(ObjectIDRef ID, bool IsNullTerminated) : Object(ObjectKind::Blob, ID) {
    Object::setSubclassData(IsNullTerminated);
  }
};

class BlobRef final : public Blob {
  void anchor() override;

public:
  StringRef getData() const override { return Data; }

  BlobRef(ObjectIDRef ID, StringRef Data, bool IsNullTerminated = false)
      : Blob(ID, IsNullTerminated), Data(Data) {}
  BlobRef(const Blob &B) : BlobRef(B.getID(), B.getData(), B.isNullTerminated()) {}

private:
  StringRef Data;
};

class InlineBlob final : public Blob {
  void anchor() override;

public:
  StringRef getData() const override { return Data; }

  InlineBlob(ObjectIDRef ID, StringRef Data, bool IsNullTerminated = false)
      : Blob(ID, IsNullTerminated), ID(ID), Data(Data.str()) {
    updateID(this->ID);
  }
  InlineBlob(const Blob &B) : BlobRef(B.getID(), B.getData(), B.isNullTerminated()) {}

private:
  ObjectID ID;
  std::string Data;
};

struct ObjectCapture {
  inline constexpr size_t DefaultMinFileSize = 16 * 1024;
};

struct BlobCapture {
  /// Structure for capturing persistent data with the same lifetime as the CAS.
  struct PersistentData {
    /// If set to true, \a Data will only be set when \a IsNullTerminated is true.
    bool RequireNullTerminated = false;

    Optional<bool> IsNullTerminated;
    Optional<StringRef> Data;
  };

  /// Structure for capturing data contained in a mapped_file_region.
  struct MappedFileData {
    /// Minimum file size for a returned mapped region.
    size_t MinFileSize = ObjectCapture::DefaultMinFileSize;

    /// If set to true, \a Data will only be set when \a IsNullTerminated is true.
    bool RequireNullTerminated = false;

    Optional<bool> IsNullTerminated;
    Optional<StringRef> Data;
    mapped_file_region File;
  };

  BlobCapture(raw_ostream &Stream) : Stream(Stream) {}

  /// Default way to capture the content of the blob. Use a raw_svector_ostream
  /// or raw_string_stream to capture in a vector or string.
  raw_ostream &Stream;

  /// Set to non-null to capture a StringRef directly, when the CAS can provide one.
  PersistentData *Persistent = nullptr;

  /// Set to non-null to support capturing data contained in an owned
  /// mapped_file_region, when the CAS can provide one.
  MappedFileData *MappedFile = nullptr;
};

class Node : public Object {
  void anchor() override;
public:
  virtual StringRef getData() const = 0;
  virtual FlatObjectIDArrayRef getReferences() const = 0;
  bool hasReferences() const;
};

class NodeRef final : public Node {
  void anchor() override;
  StringRef getData() const override;
  FlatObjectIDArrayRef getReferences() const override;

private:
  FlatObjectIDArrayRef Refs;
  StringRef Data;
};

class InlineNode final : public Node {
  void anchor() override;
  StringRef getData() const override;
  FlatObjectIDArrayRef getReferences() const override;

private:
  std::string RefStorage;
  std::string Data;
};

class FlatTreeEntryDecoder {
  virtual void anchor();

public:
  NamedTreeEntry getEntry(size_t I,
                          FlatObjectIDArrayRef IDs, ArrayRef<char> FlatData) const = 0;
  Optional<NamedTreeEntry> lookupEntry(StringRef Name,
                                       FlatObjectIDArrayRef IDs, ArrayRef<char> FlatData) const = 0;
  Error forEachEntry(function_ref<Error (const NamedTreeEntry &)> Callback,
                     FlatObjectIDArrayRef IDs, ArrayRef<char> FlatData) const = 0;
};

class FlatTreeEntriesRef {
public:
  NamedTreeEntry operator[](size_t I) const {
    return Decoder.get(I);
  }

  FlatTreeEntriesRef(FlatTreeEntryDecoder &Decoder, FlatObjectIDArrayRef IDs,
                     ArrayRef<char> OpaqueFlatData);

private:
  FlatObjectIDArrayRef IDs;
  ArrayRef<char> OpaqueFlatData;
  FlatTreeEntryDecoder *Decoder;
};

class Tree : public Object {
  void anchor() override;

public:
  bool isEmpty() const;
  virtual FlatTreeEntriesRef getEntries() const = 0;

  Tree(ObjectIDRef ID) : Object(ObjectKind::Tree, ID) {}
};

class TreeRef final : public Tree {
  void anchor() override;

public:
  FlatTreeEntriesRef getEntries() const override { return Entries; }

  TreeRef(ObjectIDRef ID, FlatTreeEntriesRef Entries)
      : Tree(ID), Entries(Entries) {}

private:
  FlatTreeEntriesRef Entries;
};

class InlineTree final : public Tree, private FlatTreeEntryDecoder {
  void anchor() override;

public:
  FlatTreeEntriesRef getEntries() const override {
    const auto *HashesStart = reinterpret_cast<const uint8_t *>(IDs.data());
    FlatObjectIDArrayRef Hashes(getID().size(),
                                makeArrayRef(HashesStart, HashesStart + IDs.size()));
    return FlatTreeEntriesRef(*this, Hashes, Data);
  }

  static InlineTree encode(size_t HashSize, ArrayRef<NamedTreeEntry> Entries);
  static InlineTree encode(size_t HashSize, FlatTreeEntriesRef Entries);

  static void encodeEntry(const NamedTreeEntry &Entry,
                          SmallVectorImpl<uint8_t> &FlatIDs,
                          SmallVectorImpl<char> &FlatEntries,
                          SmallVectorImpl<char> &FlatNames);
  static InlineTree encode(FlatTreeEntriesRef FlatIDs,
                           ArrayRef<char> FlatEntries,
                           ArrayRef<char> FlatNames);
  static NamedTreeEntry decodeEntry(size_t I, FlatTreeEntriesRef IDs, StringRef Data);

private:
  NamedTreeEntry getEntry(size_t I, FlatObjectIDArrayRef IDs, ArrayRef<char> FlatData) const override;
  Optional<NamedTreeEntry> lookupEntry(StringRef Name,
                                       FlatObjectIDArrayRef IDs, ArrayRef<char> FlatData) const override;
  Error forEachEntry(function_ref<Error (const NamedTreeEntry &)> Callback,
                     FlatObjectIDArrayRef IDs, ArrayRef<char> FlatData) const override;

private:
  std::string IDs;
  std::string Data;
};

struct NodeCapture {
  /// Structure for capturing persistent data with the same lifetime as the CAS.
  struct PersistentData {
    /// If set to true, \a Data will only be set when \a IsNullTerminated is true.
    bool RequireNullTerminated = false;

    Optional<bool> IsNullTerminated;
    Optional<StringRef> Data;
    Optional<FlatObjectIDArrayRef> Refs;
  };

  /// Structure for capturing data contained in a mapped_file_region.
  struct MappedFileData : public BlobCapture::MappedFileData {
    /// Minimum file size for a returned mapped region.
    size_t MinFileSize = ObjectCapture::DefaultMinFileSize;

    /// If set to true, only returend when \a IsNullTerminated is true.
    bool RequireNullTerminated = false;

    Optional<bool> IsNullTerminated;
    Optional<StringRef> Data;
    Optional<FlatObjectIDArrayRef> Refs;
    mapped_file_region File;
  };

  /// Structure for capturing streamed data (the default).
  struct StreamedData {
    raw_ostream &Data;
    SmallVectorImpl<ObjectID> &Refs;
  };

  NodeCapture(StreamedData &Stream) : Stream(Stream) {}
  StreamedData &Stream;
  PersistentData *Persistent = nullptr;
  MappedFileData *MappedFile = nullptr;
};

struct TreeCapture {
  /// Structure for capturing persistent data with the same lifetime as the CAS.
  struct PersistentData {
    Optional<TreeRef> Tree;
  };

  /// Structure for capturing data contained in a mapped_file_region.
  struct MappedFileData {
    /// Minimum file size for a returned mapped region.
    size_t MinFileSize = ObjectCapture::DefaultMinFileSize;

    Optional<TreeRef> Tree;
    mapped_file_region File;
  };

  /// Structure for capturing streamed data (the default).
  struct StreamedData {
    StringSaver &Strings;
    SmallVectorImpl<NamedTreeEntry> &Entries;
  };

  TreeCapture(StreamedData &Stream) : Stream(Stream) {}
  StreamedData &Stream;
  PersistentData *Persistent = nullptr;
  MappedFileData *MappedFile = nullptr;
};

class InMemoryObjectStore;
class ObjectStore {
  virtual void anchor();

public:
  virtual ~ObjectStore() = default;

  virtual Error putNode(ArrayRef<ObjectIDRef> Refs, StringRef Data, Optional<ObjectID> &ID) = 0;
  virtual Error putBlob(StringRef Data, Optional<ObjectID> &ID) = 0;

  Error putNode(ArrayRef<ObjectID> Refs, StringRef Data, Optional<ObjectID> &ID);

  virtual Error getNode(ObjectIDRef ID, std::unique_ptr<Node> &Node) = 0;
  virtual Error getBlob(ObjectIDRef ID, std::unique_ptr<Blob> &Data);
  virtual Error captureBlob(ObjectIDRef ID, BlobCapture &Capture) = 0;

  virtual std::unique_ptr<InMemoryObjectStore> createInMemoryLayer();
};

class InMemoryObjectStore : public ObjectStore {
  void anchor() override;

public:
  virtual Error putNode(ArrayRef<ObjectIDRef> Refs, StringRef Data, Optional<ObjectIDRef> &ID) = 0;
  virtual Error putBlob(StringRef Data, Optional<ObjectIDRef> &ID) = 0;

  Error putNode(ArrayRef<ObjectID> Refs, StringRef Data, Optional<ObjectIDRef> &ID);

  virtual Error getBlob(ObjectIDRef ID, Blob *&Blob) = 0;
  virtual Error getNode(ObjectIDRef ID, Node *&Node) = 0;
};

class ObjectCache {
  virtual void anchor();

public:
  virtual ~ObjectCache() = default;

  virtual Error put(StringRef Key, ObjectIDRef Value) = 0;
  virtual Error put(ObjectIDRef Key, ObjectIDRef Value) = 0;

  virtual Error get(StringRef Key, Optional<ObjectIDRef> &Value) = 0;
  virtual Error get(StringRef Key, Optional<ObjectIDRef> &Value) = 0;
  virtual Error get(ObjectIDRef Key, Optional<ObjectID> &Value) = 0;
  virtual Error get(ObjectIDRef Key, Optional<ObjectID> &Value) = 0;
};

class CASDB;

/// Wrapper around a raw hash-based identifier for a CAS object.
class CASID {
public:
  ArrayRef<uint8_t> getHash() const { return Hash; }

  friend bool operator==(CASID LHS, CASID RHS) {
    return LHS.getHash() == RHS.getHash();
  }
  friend bool operator!=(CASID LHS, CASID RHS) {
    return LHS.getHash() != RHS.getHash();
  }

  CASID() = delete;
  explicit CASID(ArrayRef<uint8_t> Hash) : Hash(Hash) {}
  explicit operator ArrayRef<uint8_t>() const { return Hash; }

  friend hash_code hash_value(cas::CASID ID) {
    return hash_value(ID.getHash());
  }

private:
  ArrayRef<uint8_t> Hash;
};

/// Generic CAS object reference.
class ObjectRef {
public:
  CASID getID() const { return ID; }
  operator CASID() const { return ID; }

protected:
  explicit ObjectRef(CASID ID) : ID(ID) {}

private:
  CASID ID;
};

/// Reference to a blob in the CAS.
class BlobRef : public ObjectRef {
public:
  /// Get the content of the blob. Valid as long as the CAS is valid.
  StringRef getData() const { return Data; }
  ArrayRef<char> getDataArray() const {
    return makeArrayRef(Data.begin(), Data.size());
  }
  StringRef operator*() const { return Data; }
  const StringRef *operator->() const { return &Data; }

  BlobRef() = delete;

private:
  BlobRef(CASID ID, StringRef Data) : ObjectRef(ID), Data(Data) {
    assert(Data.end()[0] == 0 && "Blobs should guarantee null-termination");
  }

  friend class CASDB;
  StringRef Data;
};

/// Reference to a tree CAS object. Reference is passed by value and is
/// expected to be valid as long as the \a CASDB is.
///
/// TODO: Add an API to expose a range of NamedTreeEntry.
///
/// TODO: Consider deferring copying/destruction/etc. to TreeAPI to enable an
/// implementation of CASDB to use reference counting for tree objects. Not
/// sure the utility, though, and it would add cost -- seems easier/better to
/// just make objects valid "forever".
class TreeRef : public ObjectRef {
public:
  bool empty() const { return NumEntries == 0; }
  size_t size() const { return NumEntries; }

  inline Optional<NamedTreeEntry> lookup(StringRef Name) const;
  inline NamedTreeEntry get(size_t I) const;

  /// Visit each tree entry in order, returning an error from \p Callback to
  /// stop early.
  inline Error
  forEachEntry(function_ref<Error(const NamedTreeEntry &)> Callback) const;

  TreeRef() = delete;

private:
  TreeRef(CASID ID, CASDB &CAS, const void *Tree, size_t NumEntries)
      : ObjectRef(ID), CAS(&CAS), Tree(Tree), NumEntries(NumEntries) {}

  friend class CASDB;
  CASDB *CAS;
  const void *Tree;
  size_t NumEntries;
};

/// Reference to an abstract hierarchical node, with data and references.
/// Reference is passed by value and is expected to be valid as long as the \a
/// CASDB is.
class NodeRef : public ObjectRef {
public:
  CASDB &getCAS() const { return *CAS; }

  size_t getNumReferences() const { return NumReferences; }
  inline CASID getReference(size_t I) const;

  /// Visit each reference in order, returning an error from \p Callback to
  /// stop early.
  inline Error forEachReference(function_ref<Error(CASID)> Callback) const;

  /// Get the content of the node. Valid as long as the CAS is valid.
  StringRef getData() const { return Data; }

  NodeRef() = delete;

private:
  NodeRef(CASID ID, CASDB &CAS, const void *Object, size_t NumReferences,
          StringRef Data)
      : ObjectRef(ID), CAS(&CAS), Object(Object), NumReferences(NumReferences),
        Data(Data) {}

  friend class CASDB;
  CASDB *CAS;
  const void *Object;
  size_t NumReferences;
  StringRef Data;
};

class CASDB {
public:
  /// Get a \p CASID from a \p Reference, which should have been generated by
  /// \a printCASID(). This succeeds as long as \p Reference is valid
  /// (correctly formatted); it does not refer to an object that exists, just
  /// be a reference that has been constructed correctly.
  virtual Expected<CASID> parseCASID(StringRef Reference) = 0;

  /// Print \p ID to \p OS, returning an error if \p ID is not a valid \p CASID
  /// for this CAS. If \p ID is valid for the CAS schema but unknown to this
  /// instance (say, because it was generated by another instance), this should
  /// not return an error.
  virtual Error printCASID(raw_ostream &OS, CASID ID) = 0;

  Expected<std::string> convertCASIDToString(CASID ID);

  Error getPrintedCASID(CASID ID, SmallVectorImpl<char> &Reference);

  virtual Expected<BlobRef> createBlob(StringRef Data) = 0;

  virtual Expected<TreeRef>
  createTree(ArrayRef<NamedTreeEntry> Entries = None) = 0;

  virtual Expected<NodeRef> createNode(ArrayRef<CASID> References,
                                       StringRef Data) = 0;

  /// Default implementation reads \p FD and calls \a createBlob(). Does not
  /// take ownership of \p FD; the caller is responsible for closing it.
  ///
  /// If \p Status is sent in it is to be treated as a hint. Implementations
  /// must protect against the file size potentially growing after the status
  /// was taken (i.e., they cannot assume that an mmap will be null-terminated
  /// where \p Status implies).
  ///
  /// Returns the \a CASID and the size of the file.
  Expected<BlobRef>
  createBlobFromOpenFile(sys::fs::file_t FD,
                         Optional<sys::fs::file_status> Status = None) {
    return createBlobFromOpenFileImpl(FD, Status);
  }

protected:
  virtual Expected<BlobRef>
  createBlobFromOpenFileImpl(sys::fs::file_t FD,
                             Optional<sys::fs::file_status> Status);

public:
  virtual Expected<BlobRef> getBlob(CASID ID) = 0;
  virtual Expected<TreeRef> getTree(CASID ID) = 0;
  virtual Expected<NodeRef> getNode(CASID ID) = 0;

  virtual Optional<ObjectKind> getObjectKind(CASID ID) = 0;
  virtual bool isKnownObject(CASID ID) { return bool(getObjectKind(ID)); }

  virtual Expected<CASID> getCachedResult(CASID InputID) = 0;
  virtual Error putCachedResult(CASID InputID, CASID OutputID) = 0;

  virtual void print(raw_ostream &) const {}
  void dump() const;

  virtual ~CASDB() = default;

protected:
  // Support for TreeRef.
  friend class TreeRef;
  virtual Optional<NamedTreeEntry> lookupInTree(const TreeRef &Tree,
                                                StringRef Name) const = 0;
  virtual NamedTreeEntry getInTree(const TreeRef &Tree, size_t I) const = 0;
  virtual Error forEachEntryInTree(
      const TreeRef &Tree,
      function_ref<Error(const NamedTreeEntry &)> Callback) const = 0;

  /// Build a \a BlobRef. For use by derived classes to access the private
  /// constructor of \a BlobRef. Templated as a hack to allow this to be
  /// declared before \a TreeRef.
  static BlobRef makeBlobRef(CASID ID, StringRef Data) {
    return BlobRef(ID, Data);
  }

  /// Extract the tree pointer from \p Ref. For use by derived classes to
  /// access the private pointer member. Ensures \p Ref comes from this
  /// instance.
  ///
  const void *getTreePtr(const TreeRef &Ref) const {
    assert(Ref.CAS == this);
    assert(Ref.Tree);
    return Ref.Tree;
  }

  /// Build a \a TreeRef from a pointer. For use by derived classes to access
  /// the private constructor of \a TreeRef.
  TreeRef makeTreeRef(CASID ID, const void *TreePtr, size_t NumEntries) {
    assert(TreePtr);
    return TreeRef(ID, *this, TreePtr, NumEntries);
  }

  // Support for NodeRef.
  friend class NodeRef;
  virtual CASID getReferenceInNode(const NodeRef &Ref, size_t I) const = 0;
  virtual Error
  forEachReferenceInNode(const NodeRef &Ref,
                         function_ref<Error(CASID)> Callback) const = 0;

  /// Extract the object pointer from \p Ref. For use by derived classes to
  /// access the private pointer member. Ensures \p Ref comes from this
  /// instance.
  ///
  const void *getNodePtr(const NodeRef &Ref) const {
    assert(Ref.CAS == this);
    assert(Ref.Object);
    return Ref.Object;
  }

  /// Build a \a NodeRef from a pointer. For use by derived classes to
  /// access the private constructor of \a NodeRef.
  NodeRef makeNodeRef(CASID ID, const void *ObjectPtr, size_t NumReferences,
                      StringRef Data) {
    assert(ObjectPtr);
    return NodeRef(ID, *this, ObjectPtr, NumReferences, Data);
  }
};

Optional<NamedTreeEntry> TreeRef::lookup(StringRef Name) const {
  return CAS->lookupInTree(*this, Name);
}

NamedTreeEntry TreeRef::get(size_t I) const { return CAS->getInTree(*this, I); }

Error TreeRef::forEachEntry(
    function_ref<Error(const NamedTreeEntry &)> Callback) const {
  return CAS->forEachEntryInTree(*this, Callback);
}

CASID NodeRef::getReference(size_t I) const {
  return CAS->getReferenceInNode(*this, I);
}

Error NodeRef::forEachReference(function_ref<Error(CASID)> Callback) const {
  return CAS->forEachReferenceInNode(*this, Callback);
}

Expected<std::unique_ptr<CASDB>>
createPluginCAS(StringRef PluginPath, ArrayRef<std::string> PluginArgs = None);
std::unique_ptr<CASDB> createInMemoryCAS();
Expected<std::unique_ptr<CASDB>> createOnDiskCAS(const Twine &Path);

void getDefaultOnDiskCASPath(SmallVectorImpl<char> &Path);
void getDefaultOnDiskCASStableID(SmallVectorImpl<char> &Path);

std::string getDefaultOnDiskCASPath();
std::string getDefaultOnDiskCASStableID();

} // namespace cas
} // namespace llvm

#endif // LLVM_CAS_CASDB_H
