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
  Invalid, /// Invalid object kind.
  Blob, /// Data, with no references.
  Tree, /// Filesystem-style tree, with named references and entry types.
  Node, /// Abstract hierarchical node, with data and references.
};

/// IDs:
///
/// UniqueID    - Owned ID    (small Hash optimization)
/// UniqueIDRef - Unowned ID  (wraps ArrayRef<uint8_t>)
///
///
/// For InMemoryView:
///
/// InMemoryObject *    - Common base class. Has ID and hash size and subclass data.
/// InMemoryBlob *      - Get a blob. Adds char* for Data (S32|S16=>size).
/// InMemoryNode *      - Get a node. Adds char* as Blob; Refs in uint8_t* with flat hash array.
/// InMemoryTree *      - Get a tree. Adds vtable and uint8_t*. S32=>Num.
///
/// BlobRef => InMemoryBlob* != nullptr ??
/// TreeRef => InMemoryTree* != nullptr ??
/// NodeRef => InMemoryNode* != nullptr ??
///
///
/// Storage:
/// ========
///
/// Namespace
///   - getName()                  => StringRef (e.g., "llvm.builtin.v1[sha1]")
///   - printID                    => raw_ostream&
///   - parseID                    => StringRef => UniqueID
///   - getHashSize()              => size_t (e.g., 20 or 32)
///
/// ObjectHasher
///   - Hash objects
///   - identify{Blob,Node,Tree}   => UniqueID
///   - identify{Blob,Node,Tree}   => UniqueIDRef + out-param: SmallVectorImpl<uint8_t>&
///   - Eventually: overloads for identify() with continuation out-param
///   - Eventually: overloads for identify() with std::future out-param
///
/// ObjectStore : ObjectHasher
///   - Store objects
///   - get{Blob,Node,Tree}        => Out-param: Capture
///   - create{Blob,Node,Tree}     => Out-param: UniqueID
///   - create{Blob,Node,Tree}     => Out-param: UniqueID + Capture
///   - createBlobFromOnDiskPath   => Out-param: UniqueID + Capture
///   - createBlobFromOpenFile     => Out-param: UniqueID + Capture
///   - Eventually: overloads with continuation out-param
///   - Eventually: overloads with std::future out-param
///
/// InMemoryView : ObjectStore
///   - View of objects; wraps any *other* ObjectStore
///   - get{Blob,Node,Tree}        => InMemoryBlob*,InMemoryNode*,InMemoryTree*
///   - create{Blob,Node,Tree}     => InMemoryBlob*,InMemoryNode*,InMemoryTree*
///   - Eventually: overloads with continuation out-param
///   - Eventually: overloads with std::future out-param
///
/// BuiltinCAS : ObjectStore
///   - Implements (final) hasher part of API
///
/// BuiltinFileBasedCAS : BuiltinCAS
/// BuiltinDatabaseCAS : BuiltinCAS
/// (etc.)
/// BuiltinNullCAS : BuiltinCAS
///   - get() fails
///   - create() returns out parameters but has no side effects
///   - use with InMemoryView for an "in-memory CAS"
///
///
/// Caching:
/// ========
///
/// ActionCache
///   - Constructed with a Namespace: needs to know size of hash
///   - getIDSize()                                      => size_t
///   - put: (StringRef, UniqueIDRef)                    => Error
///   - get: (StringRef,
///           SmallVectorImpl<uint8_t> &UniqueIDStorage) => Expected<UniqueIDRef>
///   - get: (StringRef, Optional<UniqueID>&)            => Expected<UniqueID>
///
/// OnDiskActionCache
///   - Key-value store on disk
///   - For now: hash StringRef and use OnDiskHashMappedTrie
///   - Eventually: compare against alternatives
///
/// InMemoryActionCache
///   - Key-value store in memory
///   - For now: hash StringRef and use ThreadSafeHashMappedTrie
///   - Eventually: compare against sharding + locks
///
///
/// Plugins:
/// ========
///
/// - InMemoryView does not have plugins
///     - Stores mapped memory and provies views of trees/blobs/etc.
///
/// - ObjectStorePlugin : ObjectStore
///     - Move BuiltinCAS and subclasses into a plugin
///     - Plugins should be out-of-process (in a daemon)
///     - Use memory mapped IPC to avoid overhead / keep BuiltinCAS fast
///     - Maybe: add an "in-memory store" plugin that's ephemeral, lasting just
///       until the daemon shuts down but sharing work as long as the daemon
///       lives
///
/// - ActionCachePlugin : ActionCache
///     - Move OnDiskActionCache into a plugin
///     - Plugins should be out-of-process (in a daemon)
///     - Maybe: add an "in-memory cache" plugin that's ephemeral, lasting just
///       until the daemon shuts down but sharing work as long as the daemon
///       lives
///

class UniqueID;
class UniqueIDRef;
class Namespace {
  virtual void anchor();

public:
  StringRef getName() const { return Name; }
  size_t getHashSize() const { return HashSize; }

  /// Print an ID.
  virtual void printID(const UniqueIDRef &ID, raw_ostream &OS) const = 0;

  /// Parse an ID.
  virtual Error parseID(StringRef Reference, UniqueID &ID) const = 0;

  Namespace() = delete;
  Namespace(Namespace &&) = delete;
  Namespace(const Namespace &) = delete;

protected:
  Namespace(StringRef Name, size_t HashSize)
      : Name(Name.str()), HashSize(HashSize) {
    assert(HashSize >= sizeof(size_t) && "Expected strong hash");
  }

private:
  std::string Name;
  const size_t HashSize;
};

class UniqueIDRef {
  // Fixed set of expected sizes for hashes. Allows comparing hashes without
  // chasing the Namespace pointer. Other sizes are still okay, but slower.
  enum HashSize {
    NotEmbedded,
    Embedded20B, // 160 bits
    Embedded32B, // 256 bits
    // Can fill common values can go here.
  };
  enum : size_t {
    // Expect hashes to be at least 16B.
    MinHashSize = 16,
  };

public:
  size_t getHashSize() const {
    assert(NamespaceAndHashSize.getPointer() && "Expected valid namespace");

    HashSize HS = NamespaceAndHashSize.getInt();
    if (HS == Embedded20B)
      return 20;
    if (HS == Embedded32B)
      return 32;
    return NamespaceAndHashSize.getPointer()->getHashSize();
  }

  const Namespace &getNamespace() const {
    assert(NamespaceAndHashSize.getPointer() && "Expected valid namespace");
    return *NamespaceAndHashSize.getPointer();
  }

  ArrayRef<uint8_t> getHash() const {
    return makeArrayRef(Hash, Hash + getHashSize());
  }

  friend bool operator==(UniqueIDRef LHS, UniqueIDRef RHS) {
    if (LHS.NamespaceAndHashSize != RHS.NamespaceAndHashSize)
      return false;
    if (LHS.Hash == RHS.Hash)
      return true;
    if (makeArrayRef(LHS.Hash, MinHashSize) !=
        makeArrayRef(RHS.Hash, MinHashSize))
      return false;
    return LHS.getHash().drop_front(MinHashSize) ==
           RHS.getHash().drop_front(MinHashSize);
  }
  friend bool operator!=(UniqueIDRef LHS, UniqueIDRef RHS) {
    return !(LHS == RHS);
  }

  uint64_t getHashValue() const {
    uint64_t Value = 0;
    for (size_t I = 0, E = sizeof(uint64_t); I != E; ++I)
      Value |= Hash[I] << (I * 8);
    return Value;
  }

  UniqueIDRef() = delete;
  UniqueIDRef(UniqueIDRef &&) = default;
  UniqueIDRef(const UniqueIDRef &) = default;
  UniqueIDRef &operator=(UniqueIDRef &&) = default;
  UniqueIDRef &operator=(const UniqueIDRef &) = default;

  UniqueIDRef(const Namespace &NS, ArrayRef<uint8_t> Hash) : NS(&NS), Hash(Hash.begin()) {
    assert(NS.getHashSize() >= MinHashSize && "Expected hash not to be small");
    assert(NS.getHashSize() == Hash.size() && "Expected valid hash for namespace");
    if (NS.getHashSize() == 20)
      NS.setInt(Embedded20B);
    else if (NS.getHashSize() == 32)
      NS.setInt(Embedded32B);
  }

  struct DenseMapTombstoneTag {};
  struct DenseMapEmptyTag {};

  explicit UniqueIDRef(DenseMapTombstoneTag) :
      : Hash(DenseMapInfo<ArrayRef<uint8_t>>::getTombstoneKey());
  explicit UniqueIDRef(DenseMapEmptyTag) :
      : Hash(DenseMapInfo<ArrayRef<uint8_t>>::getEmptyKey());

private:
  PointerIntPair<const Namespace *, 3> NamespaceAndHashSize;
  const uint8_t *Hash = nullptr;
};

raw_ostream &operator<<(raw_ostream &OS, UniqueIDRef ID) {
  ID.getNamespace().printID(ID, OS);
  return OS;
}

hash_code hash_value(UniqueIDRef ID) { return hash_code(ID.getHashValue()); }

} // namespace cas

template <> struct DenseMapInfo<cas::UniqueIDRef> {
  static cas::UniqueIDRef getEmptyKey() {
    return cas::UniqueIDRef(cas::UniqueIDRef::DenseMapEmptyTag{});
  }

  static cas::UniqueIDRef getTombstoneKey() {
    return cas::UniqueIDRef(cas::UniqueIDRef::DenseMapTombstoneTag{});
  }

  static unsigned getHashValue(cas::UniqueIDRef ID) { return ID.getHashValue(); }

  static bool isEqual(cas::UniqueIDRef LHS, cas::UniqueIDRef RHS) {
    return DenseMapInfo<ArrayRef<uint8_t>>::isEqual(LHS.getHash(),
                                                    RHS.getHash());
  }
};

namespace cas {

/// A unique ID that owns its hash storage. Uses small string optimization for
/// hashes under 32B.
///
/// May be uninitialized, possibly because it was moved-away from.
class UniqueID {
public:
  explicit operator bool() const { return NS; }

  const Namespace &getNamespace() const {
    assert(NS && "Invalid namespace");
    return *NS;
  }
  ArrayRef<uint8_t> getHash() const {
    const uint8_t *HashPtr = isSmall() ? Storage.Hash : Storage.Mem;
    return makeArrayRef(HashPtr, HashPtr + getNamespace().getHashSize());
  }

  operator UniqueIDRef() const { return UniqueIDRef(getNamespace(), getHash()); }

  UniqueID() = default;
  UniqueID(UniqueID &&ID) { moveImpl(ID); }
  UniqueID(const UniqueID &ID) {
    if (ID)
      copyImpl(ID);
  }

  UniqueID &operator=(UniqueID &&ID) {
    destroy();
    return moveImpl(ID);
  }
  UniqueID &operator=(const UniqueID &ID) {
    if (ID)
      return copyImpl(ID);
    destroy();
    NS = nullptr;
    return *this;
  }

  UniqueID(UniqueIDRef ID) { copyImpl(ID); }
  UniqueID &operator=(UniqueIDRef ID) {
    return copyImpl(ID);
  }

  ~UniqueID() { destroy(); }

private:
  void allocate() {
    if (!isSmall())
      Storage.Mem = new uint8_t[NS->getHashSize()];
  }
  void destroy() {
    if (!isSmall())
      delete[] Storage.Mem;
  }
  UniqueID &copyImpl(UniqueIDRef ID) {
    if (NS != &ID.getNamespace()) {
      destroy();
      NS = &ID.getNamespace();
      allocate();
    }
    memcpy(isSmall() ? Storage.Hash : Storage.Mem, Hash, Hash.size());
    return *this;
  }
  UniqueID &moveImpl(UniqueID &ID) {
    NS = ID.NS;
    memcpy(&Storage, &ID.Storage, sizeof(Storage));
    ID.NS = nullptr;
    memset(&ID.Storage, sizeof(Storage), 0);
    return *this;
  }
  constexpr inline size_t InlineHashSize = 32;
  bool isSmall() const {
    return !NS || NS->getNamespace().getHashSize() <= InlineHashSize;
  }

  const Namespace *NS = nullptr;
  union {
    const uint8_t *Mem = nullptr;
    uint8_t Hash[InlineHashSize];
  } Storage;
};

class FlatUniqueIDArrayRef {
public:
  class iterator : iterator_facade_base<iterator, std::random_access_iterator_tag, const UniqueIDRef,
                                        ptrdiff_t, const UniqueIDRef *, UniqueIDRef> {
    // FIXME: Merge from main, which already has this, and remove this copy's
    // operator->() in favour of the default implementation in
    // iterator_facade_base.
    class PointerProxy {
      friend iterator;
      UniqueIDRef ID;

    public:
      const UniqueIDRef *operator->() const { return &ID; }
    };

  public:
    UniqueIDRef operator*() const { return UniqueIDRef(makeArrayRef(I, I + Size)); }
    PointerProxy operator->() const { return PointerProxy{**this}; }
    iterator &operator++() { return *this += 1; }
    iterator &operator--() { return *this -= 1; }
    iterator &operator+=(ptrdiff_t Diff) {
      I += Diff * HashSize;
      return *this;
    }
    iterator &operator-=(ptrdiff_t Diff) {
      I -= Diff * HashSize;
      return *this;
    }
    bool operator<(iterator RHS) const { return I < RHS.I; }
    ptrdiff_t operator-(iterator RHS) const { return (I - RHS.I) / HashSize; }

  private:
    friend FlatUniqueIDArrayRef;
    iterator() = delete;
    iterator(size_t HashSize, const uint8_t *I) : HashSize(HashSize), I(I) {}
    size_t HashSize;
    const uint8_t *I;
  };

  iterator begin() const { return iterator(HashSize, Hashes.begin()); }
  iterator end() const { return iterator(HashSize, Hashes.end()); }
  size_t size() const { return Hashes.size() / HashSize; }
  bool empty() const { return Hashes.empty(); }
  UniqueIDRef operator[](ptrdiff_t I) const { return begin()[I]; }

  const Namespace &getNamespace() const { return Namespace; }
  size_t getHashSize() const { return HashSize; }
  ArrayRef<uint8_t> getFlatHashes() const { return FlatHashes; }

  FlatUniqueIDArrayRef(const Namespace &NS, ArrayRef<uint8_t> FlatHashes)
      : NS(&NS), FlatHashes(FlatHashes) {
    size_t HashSize = NS.getHashSize();
    assert(FlatHashes.size() / HashSize * HashSize == FlatHashes.size() &&
           "Expected contiguous hashes");
  }

private:
  const Namespace *NS = nullptr;
  ArrayRef<uint8_t> FlatHashes;
};

class ObjectRef {
public:
  explicit operator bool() const { return NS; }

  const Namespace &getNamespace() const { return *NS; }
  UniqueIDRef getID() const {
    return UniqueIDRef(getNamespace(), getHash());
  }
  ArrayRef<uint8_t> getHash() const {
    return makeArrayRef(Hash, Hash + getNamespace().getHashSize());
  }

protected:
  ObjectRef() = default;
  explicit ObjectRef(UniqueIDRef ID)
      : NS(&ID.getNamespace()), Hash(ID.getHash().begin()) {}

private:
  const Namespace *NS = nullptr;
  const uint8_t *Hash = nullptr;
};

class BlobRef : public ObjectRef {
public:
  StringRef getData() const { return Data; }
  ArrayRef<uint8_t> getDataArray() const {
    const uint8_t *Ptr = reinterpret_cast<const uint8_t *>(Data);
    return makeArrayRef(Ptr, Ptr + getSize());
  }

  BlobRef() = default;
  BlobRef(UniqueIDRef ID, StringRef Data) : ObjectRef(ID), Data(Data.data()) {
    assert(Data.end()[0] == 0 && "Expected null-terminated data");
  }

private:
  StringRef Data;
};

class NodeRef : public ObjectRef {
public:
  StringRef getData() const { return Data; }
  ArrayRef<uint8_t> getDataArray() const {
    const uint8_t *Ptr = reinterpret_cast<const uint8_t *>(Data);
    return makeArrayRef(Ptr, Ptr + getSize());
  }

  FlatUniqueIDArrayRef getHashes() const {
    return FlatUniqueIDArrayRef(getNamespace(), FlatHashes);
  }

  NodeRef() = default;
  NodeRef(UniqueIDRef ID, StringRef Data, FlatUniqueIDArrayRef Hashes)
       : ObjectRef(ID), Data(Data.data()), FlatHashes(Hashes.getFlatHashes()) {
    assert(Data.end()[0] == 0 && "Expected null-terminated data");
    assert(&ID.getNamespace() == &Hashes.getNamespace() &&
           "Mismatched namespace");
  }

private:
  StringRef Data;
  ArrayRef<uint8_t> FlatHashes;
};

class NodeRef : public ObjectRef {
public:
  StringRef getData() const { return StringRef(Data, getSize()); }
  ArrayRef<uint8_t> getDataArray() const {
    return makeArrayRef(reinterpret_cast<const uint8_t *>(Data),
                        reinterpret_cast<const uint8_t *>(Data) + getSize());
  }
  uint64_t getSize() const {
    return uint64_t(getSubclassData32()) | uint64_t(getSubclassData16()) << 32;
  }

  NodeRef() = default;
  NodeRef(UniqueIDRef ID, StringRef Data, FlatUniqueIDArrayRef Hashes)
      : ObjectRef(ID), Data(Data.data()), FlatHashes(Hashes.getFlatHashes()) {
    assert(ID.getHashSize() == Hashes.getHashSize() &&
           "Expected ID namespace to match");
    assert(Data.end()[0] == 0 && "Expected null-terminated data");
    assert(getSize() == Data.size() && "Ran out of size bits");
  }

private:
  const char *Data = nullptr;
  ArrayRef<uint8_t> FlatHashes;
};

struct ObjectCapture {
  inline constexpr size_t DefaultMinFileSize = 16 * 1024;
};

struct ObjectStore {
  Error getBlob(UniqueIDRef ID,
                function_ref<BlobCapture (size_t Size, bool IsNullTerminated)>);
};

struct BlobCapture {
  /// Structure for capturing data contained in a mapped_file_region.
  struct MemoryMapped {
    /// Minimum file size for a returned mapped region.
    size_t MinFileSize = ObjectCapture::DefaultMinFileSize;

    /// If set to true, \a Data will only be set when \a IsNullTerminated is
    /// true.
    bool RequireNullTerminated = false;

    Optional<bool> IsNullTerminated;
    Optional<StringRef> Data;
    mapped_file_region File;
  };

  BlobCapture(raw_ostream &Stream) : Stream(Stream) {}

  /// Default way to capture the content of the blob. Use a raw_svector_ostream
  /// or raw_string_stream to capture in a vector or string.
  raw_ostream &Stream;

  /// Set to non-null to support capturing data contained in an owned
  /// mapped_file_region, when the CAS can provide one.
  MemoryMapped *MappedFile = nullptr;
};

class Node : public Object {
  void anchor() override;
public:
  virtual StringRef getData() const = 0;
  virtual FlatUniqueIDArrayRef getReferences() const = 0;
  bool hasReferences() const;
};

class NodeRef final : public Node {
  void anchor() override;
  StringRef getData() const override;
  FlatUniqueIDArrayRef getReferences() const override;

private:
  FlatUniqueIDArrayRef Refs;
  StringRef Data;
};

class InlineNode final : public Node {
  void anchor() override;
  StringRef getData() const override;
  FlatUniqueIDArrayRef getReferences() const override;

private:
  std::string RefStorage;
  std::string Data;
};

class FlatTreeEntryDecoder {
  virtual void anchor();

public:
  NamedTreeEntry getEntry(size_t I,
                          FlatUniqueIDArrayRef IDs, ArrayRef<char> FlatData) const = 0;
  Optional<NamedTreeEntry> lookupEntry(StringRef Name,
                                       FlatUniqueIDArrayRef IDs, ArrayRef<char> FlatData) const = 0;
  Error forEachEntry(function_ref<Error (const NamedTreeEntry &)> Callback,
                     FlatUniqueIDArrayRef IDs, ArrayRef<char> FlatData) const = 0;
};

class FlatTreeEntryArrayRef {
public:
  NamedTreeEntry operator[](size_t I) const {
    return Decoder.get(I);
  }

  FlatTreeEntryArrayRef(FlatTreeEntryDecoder &Decoder, FlatUniqueIDArrayRef IDs,
                     ArrayRef<char> OpaqueFlatData);

private:
  FlatUniqueIDArrayRef IDs;
  ArrayRef<char> OpaqueFlatData;
  FlatTreeEntryDecoder *Decoder;
};

class Tree : public Object {
  void anchor() override;

public:
  bool isEmpty() const;
  virtual FlatTreeEntryArrayRef getEntries() const = 0;

  Tree(ObjectRef ID) : Object(ObjectKind::Tree, ID) {}
};

class TreeRef final : public Tree {
  void anchor() override;

public:
  FlatTreeEntryArrayRef getEntries() const override { return Entries; }

  TreeRef(ObjectRef ID, FlatTreeEntryArrayRef Entries)
      : Tree(ID), Entries(Entries) {}

private:
  FlatTreeEntryArrayRef Entries;
};

class InlineTree final : public Tree, private FlatTreeEntryDecoder {
  void anchor() override;

public:
  FlatTreeEntryArrayRef getEntries() const override {
    const auto *HashesStart = reinterpret_cast<const uint8_t *>(IDs.data());
    FlatUniqueIDArrayRef Hashes(getID().size(),
                                makeArrayRef(HashesStart, HashesStart + IDs.size()));
    return FlatTreeEntryArrayRef(*this, Hashes, Data);
  }

  static InlineTree encode(size_t HashSize, ArrayRef<NamedTreeEntry> Entries);
  static InlineTree encode(size_t HashSize, FlatTreeEntryArrayRef Entries);

  static void encodeEntry(const NamedTreeEntry &Entry,
                          SmallVectorImpl<uint8_t> &FlatIDs,
                          SmallVectorImpl<char> &FlatEntries,
                          SmallVectorImpl<char> &FlatNames);
  static InlineTree encode(FlatTreeEntryArrayRef FlatIDs,
                           ArrayRef<char> FlatEntries,
                           ArrayRef<char> FlatNames);
  static NamedTreeEntry decodeEntry(size_t I, FlatTreeEntryArrayRef IDs, StringRef Data);

private:
  NamedTreeEntry getEntry(size_t I, FlatUniqueIDArrayRef IDs, ArrayRef<char> FlatData) const override;
  Optional<NamedTreeEntry> lookupEntry(StringRef Name,
                                       FlatUniqueIDArrayRef IDs, ArrayRef<char> FlatData) const override;
  Error forEachEntry(function_ref<Error (const NamedTreeEntry &)> Callback,
                     FlatUniqueIDArrayRef IDs, ArrayRef<char> FlatData) const override;

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
    Optional<FlatUniqueIDArrayRef> Refs;
  };

  /// Structure for capturing data contained in a mapped_file_region.
  struct MemoryMapped : public BlobCapture::MemoryMapped {
    /// Minimum file size for a returned mapped region.
    size_t MinFileSize = ObjectCapture::DefaultMinFileSize;

    /// If set to true, only returend when \a IsNullTerminated is true.
    bool RequireNullTerminated = false;

    Optional<bool> IsNullTerminated;
    Optional<StringRef> Data;
    Optional<FlatUniqueIDArrayRef> Refs;
    mapped_file_region File;
  };

  /// Structure for capturing streamed data (the default).
  struct StreamedData {
    raw_ostream &Data;
    SmallVectorImpl<UniqueID> &Refs;
  };

  NodeCapture(StreamedData &Stream) : Stream(Stream) {}
  StreamedData &Stream;
  PersistentData *Persistent = nullptr;
  MemoryMapped *MappedFile = nullptr;
};

struct TreeCapture {
  /// Structure for capturing persistent data with the same lifetime as the CAS.
  struct PersistentData {
    Optional<TreeRef> Tree;
  };

  /// Structure for capturing data contained in a mapped_file_region.
  struct MemoryMapped {
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
  MemoryMapped *MappedFile = nullptr;
};

class InMemoryObjectStore;
class ObjectStore {
  virtual void anchor();

public:
  virtual ~ObjectStore() = default;

  virtual Error putNode(ArrayRef<ObjectRef> Refs, StringRef Data, Optional<UniqueID> &ID) = 0;
  virtual Error putBlob(StringRef Data, Optional<UniqueID> &ID) = 0;

  Error putNode(ArrayRef<UniqueID> Refs, StringRef Data, Optional<UniqueID> &ID);

  virtual Error getNode(ObjectRef ID, std::unique_ptr<Node> &Node) = 0;
  virtual Error getBlob(ObjectRef ID, std::unique_ptr<Blob> &Data);
  virtual Error captureBlob(ObjectRef ID, BlobCapture &Capture) = 0;

  virtual std::unique_ptr<InMemoryObjectStore> createInMemoryLayer();
};

class InMemoryObjectStore : public ObjectStore {
  void anchor() override;

public:
  virtual Error putNode(ArrayRef<ObjectRef> Refs, StringRef Data, Optional<ObjectRef> &ID) = 0;
  virtual Error putBlob(StringRef Data, Optional<ObjectRef> &ID) = 0;

  Error putNode(ArrayRef<UniqueID> Refs, StringRef Data, Optional<ObjectRef> &ID);

  virtual Error getBlob(ObjectRef ID, Blob *&Blob) = 0;
  virtual Error getNode(ObjectRef ID, Node *&Node) = 0;
};

class ObjectCache {
  virtual void anchor();

public:
  virtual ~ObjectCache() = default;

  virtual Error put(StringRef Key, ObjectRef Value) = 0;
  virtual Error put(ObjectRef Key, ObjectRef Value) = 0;

  virtual Error get(StringRef Key, Optional<ObjectRef> &Value) = 0;
  virtual Error get(StringRef Key, Optional<ObjectRef> &Value) = 0;
  virtual Error get(ObjectRef Key, Optional<UniqueID> &Value) = 0;
  virtual Error get(ObjectRef Key, Optional<UniqueID> &Value) = 0;
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
