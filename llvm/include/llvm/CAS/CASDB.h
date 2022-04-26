//===- llvm/CAS/CASDB.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CAS_CASDB_H
#define LLVM_CAS_CASDB_H

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CAS/CASID.h"
#include "llvm/CAS/CASReference.h"
#include "llvm/CAS/TreeEntry.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h" // FIXME: Split out sys::fs::file_status.
#include <cstddef>

namespace llvm {

class MemoryBuffer;

namespace cas {

class CASDB;

/// Kind of CAS object.
///
/// FIXME: Remove.
enum class ObjectKind {
  /// A node, with data and zero or more references.
  Node,

  /// Data, with no references.
  ///
  /// FIXME: Move into a Filesystem schema.
  Blob,

  /// Filesystem tree, with named references and entry types.
  ///
  /// FIXME: Move into a Filesystem schema.
  Tree,
};

/// Kind of CAS reference.
enum class ReferenceKind {
  /// Raw data. Not a first-class object.
  RawData,

  /// First-class object.
  Object,
};

class BlobProxy;
class NodeProxy;
class TreeProxy;

/// Content-addressable storage for objects.
///
/// Conceptually, objects are stored in a "unique set".
///
/// - Objects are immutable ("value objects") that are defined by their
///   content. They are implicitly deduplicated by content.
/// - Each object has a unique identifier (UID) that's derived from its content,
///   called a \a CASID.
///     - This UID is a fixed-size (strong) hash of the transitive content of a
///       CAS object.
///     - It's comparable between any two CAS instances that have the same \a
///       CASIDContext::getHashSchemaIdentifier().
///     - The UID can be printed (e.g., \a CASID::toString()) and it can parsed
///       by the same or a different CAS instance with \a CASDB::parseID().
/// - An object can be looked up by content or by UID.
///     - \a storeNode(), \a storeBlob(), and \a storeTree() are "get-or-create"
///       methods, writing an object if it doesn't exist yet, and return a
///       handle to it in any case.
///     - \a loadObject(const CASID&) looks up an object by its UID.
/// - Objects can reference other objects, forming an arbitrary DAG.
///
/// There are currently three kinds of objects.
///
/// - ObjectKind::Node: Contains 0+ bytes of data and a list of 0+ \a
///   Reference. A \a Reference either points at another CAS object (\a
///   ReferenceKind::Object) or at raw data (\a ReferenceKind::RawData).
///   Created with \a storeNode().
/// - ObjectKind::Tree: Sorted list of 0+ \a NamedTreeEntry, which associates a
///   name, another CAS object, and a \a TreeEntry::EntryKind. Designed to
///   represent a filesystem in the CAS. Created with \a storeTree().
/// - ObjectKind::Blob: Contains 0+ bytes of data. Designed to represent an
///   opaque file. Created with \a storeBlob().
///
/// TODO: Remove Blob and Tree, moving these concepts to a filesystem schema
/// that sits on top of the CAS.
///
/// \a storeData() stores raw data of 0+ bytes for uniquing without requesting
/// a UID. It's appropriate when a UID will not be needed because it will only
/// be referenced directly (or by content), never by \a CASID.
///
/// - A CAS schema is free to fall back to blobs or leaf nodes for all raw data
///   requests (default), or for all requests whose content matches a
///   particular pattern (e.g., ends with null, bigger than a particular size).
/// - The CAS schema's choice may leak into the parent context. \a storeData()
///   returns \a AnyDataHandle, which could be raw data or a CAS object handle.
/// - CAS implementations must consistent. E.g., \c storeData("D1") should have
///   a consistent return across the schema (either \a ReferenceKind::Object or
///   \a ReferenceKind::RawData), regardless of whether \c storeNode(None,"D1")
///   has been called at some point. Further, the UID of a node N1 that
///   references that raw data "D1" must be consistent.
/// - Conceptually, raw data is "part of" any object that references. Raw data
///   must be serialized and hashed as part of the parent object, since it can
///   only be referenced by content (it has no UID). As such, it's implicitly
///   loaded any time an object that directly references it is loaded.
///
/// The \a CASDB interface has a few ways of referencing objects and raw data
/// in the CAS.
///
/// - \a Reference: a reference to an object or raw data in this CAS instance,
///   obtained through: \a CASDB::readRef() or \a CASDB::forEachRef(), which
///   are accessors for node CAS objects; or \a CASDB::getReference(), which
///   looks up a \a CASID without loading the object. A \a Reference points at
///   a node that is known to exist in this CAS instance, but it may not be
///   loaded (in a distributed CAS, it may not even be local!).
///     - \a Handle: a handle for a *loaded* object or raw data in this CAS
///       instance. A subclass of \a Reference.
///         - \a ObjectHandle: a handle for a CAS object.
///             - \a NodeHandle: a handle for a node. Returned by \a
///               storeNode() and the variant accessors.
///             - \a BlobHandle: a handle for a blob. Returned by \a
///               storeBlob() and the variant accessors.
///             - \a TreeHandle: a handle for a tree. Returned by \a
///               storeTree() and the variant accessors.
///             - \a AnyObjectHandle: a variant between \a ObjectHandle and its
///               non-variant subclasses. Returned by \a loadObject().
///         - \a RawDataHandle: a handle for raw data. Returned by the variant
///           accessors.
///         - \a AnyHandle: a variant between \a Handle and its non-variant
///           subclasses. Returned by \a load().
///         - \a AnyDataHandle: a variant between \a RawDataHandle, \a
///           NodeHandle (via \a NodeHandle::getData()), and \a BlobHandle (via
///           \a BlobHandle::getData()). Returned by \a storeData().
/// - \a CASID: the UID for an object in the CAS, obtained through \a
///   CASDB::getObjectID() or \a CASDB::parseID(). This is a valid CAS
///   identifier, but may reference an object that is unknown to this CAS
///   instance.
///
/// TODO: Once blobs and trees are removed, this picture will be simpler, since
/// \a ObjectHandle and \a NodeHandle can be collapsed together.
///
/// \a getOrCreateObject() can be called on any \a Reference. If it references
/// an object, this will return a handle to it. If it references raw data, this
/// will store an object that has the data. In the latter case, the returned \a
/// AnyObjectHandle will not be the same as the original \a Reference.
///
/// TODO: Consider dropping "raw data" / non-object data.
///
/// - It's very complicated to reason about.
///     - Introduces many opportunities for implementation bugs.
///     - Introduces many complications in the API. With this gone and blobs
///       and trees removed, we'd just have: \a ObjectRef and \a ObjectHandle
///       (and \a CASID); no need for any variants! Only one type of thing
///       stored!
/// - It may not be critical to performance.
///     - Implementations can do fast lookups of small objects by adding a
///       content-based index for them (prefix tree / suffix tree of content),
///       amortizing overhead of hash computation in \a storeNode().
///     - Implementations could remove small leaf objects from the main index,
///       indexing them separately with a partial hash (e.g., 4B prefix), to
///       optimize storage overhead (32B hash is big for small objects!).
///       Lookups by UID that miss the main index would get more expensive,
///       requiring a hash computation for each small object with a matching
///       partial hash, but maybe this would be rare. To mitigate this cost,
///       small leaf objects could get added to the main index lazily on first
///       lookup-by-UID, lazily adding the full overhead of the hash storage
///       only when used by clients.
///     - \a NodeBuilder and \a NodeReader interfaces can bring some of the
///       same gains without adding complexity to \a CASDB. E.g., \a
///       NodeBuilder could have an API to add a named field to a node under
///       construction; if the name is small enough, it's stored locally in the
///       node's own data, but if it's bigger then it's outlined to a separate
///       CAS object. \a NodeReader could handle the complications of reading.
///
/// FIXME: Split out ActionCache as a separate concept, and rename this
/// ObjectStore.
class CASDB : public CASIDContext {
  void anchor() override;

public:
  /// Get a \p CASID from a \p Reference, which should have been generated by
  /// \a CASID::print(). This succeeds as long as \p Reference is valid
  /// (correctly formatted); it does not refer to an object that exists, just
  /// be a reference that has been constructed correctly.
  virtual Expected<CASID> parseID(StringRef Reference) = 0;

  /// FIXME: Remove these.
  Expected<BlobProxy> createBlob(StringRef Data);
  Expected<TreeProxy> createTree(ArrayRef<NamedTreeEntry> Entries = None);
  Expected<NodeProxy> createNode(ArrayRef<CASID> References, StringRef Data);

  virtual Expected<BlobHandle> storeBlob(StringRef Data) = 0;
  virtual Expected<TreeHandle> storeTree(ArrayRef<NamedTreeEntry> Entries) = 0;
  virtual Expected<NodeHandle> storeNode(ArrayRef<Reference> Refs,
                                         ArrayRef<char> Data) = 0;

protected:
  /// Store \p Data in the CAS, without a requirement to reference it by \a
  /// CASID later.
  ///
  /// Default is to call \a storeNode(). Subclasses can override to prefer a
  /// separate table that has less overhead than first-class objects.
  ///
  /// If overridden, also override \a createObjectFromRawData().
  virtual Expected<AnyDataHandle> storeDataImpl(ArrayRef<char> Data);

public:
  Expected<AnyDataHandle> storeData(ArrayRef<char> Data) {
    return storeDataImpl(Data);
  }
  Expected<AnyDataHandle> storeData(StringRef Data) {
    return storeDataImpl(arrayRefFromStringRef<char>(Data));
  }
  Expected<AnyDataHandle> storeData(ArrayRef<uint8_t> Data) {
    return storeData(toStringRef(Data));
  }

  /// FIXME: Delete and update callers.
  Expected<BlobProxy>
  createBlobFromOpenFile(sys::fs::file_t FD,
                         Optional<sys::fs::file_status> Status = None);

  /// Default implementation reads \p FD and calls \a storeBlob(). Does not
  /// take ownership of \p FD; the caller is responsible for closing it.
  ///
  /// If \p Status is sent in it is to be treated as a hint. Implementations
  /// must protect against the file size potentially growing after the status
  /// was taken (i.e., they cannot assume that an mmap will be null-terminated
  /// where \p Status implies).
  ///
  /// Returns the \a CASID and the size of the file.
  Expected<BlobHandle>
  storeBlobFromOpenFile(sys::fs::file_t FD,
                        Optional<sys::fs::file_status> Status = None) {
    return storeBlobFromOpenFileImpl(FD, Status);
  }

protected:
  virtual Expected<BlobHandle>
  storeBlobFromOpenFileImpl(sys::fs::file_t FD,
                            Optional<sys::fs::file_status> Status);

  /// Allow CASDB implementations to create internal handles.
#define MAKE_CAS_HANDLE_CONSTRUCTOR(HandleKind)                                \
  HandleKind make##HandleKind(uint64_t InternalRef) const {                    \
    return HandleKind(this, InternalRef);                                      \
  }
  MAKE_CAS_HANDLE_CONSTRUCTOR(Handle)
  MAKE_CAS_HANDLE_CONSTRUCTOR(NodeHandle)
  MAKE_CAS_HANDLE_CONSTRUCTOR(TreeHandle)
  MAKE_CAS_HANDLE_CONSTRUCTOR(BlobHandle)
  MAKE_CAS_HANDLE_CONSTRUCTOR(RawDataHandle)
#undef MAKE_CAS_HANDLE_CONSTRUCTOR

public:
  /// Get an ID for \p Ref, if it's a first-class object.
  Optional<CASID> getObjectID(Reference Ref) const {
    return getObjectIDImpl(Ref);
  }

  /// Get an ID for \p Object.
  CASID getObjectID(const ObjectHandle &Object) const {
    Optional<CASID> ID = getObjectIDImpl(Reference(Object));
    assert(ID && "Expected Object to have an ID");
    return *ID;
  }

protected:
  virtual Optional<CASID> getObjectIDImpl(Reference Ref) const = 0;

public:
  /// Get a reference to the object called \p ID.
  ///
  /// Returns \c None if not stored in this CAS.
  virtual Optional<Reference> getReference(const CASID &ID) = 0;

  /// Get the kind of \p Ref.
  virtual ReferenceKind getReferenceKind(Reference Ref) const = 0;

  /// Load the object or get the raw data referenced by \p Ref.
  ///
  /// Errors if the object cannot be loaded.
  virtual Expected<AnyHandle> load(Reference Ref) = 0;

  /// Get the raw data referenced by \p Ref, or \c None if it's a first-class
  /// object.
  ///
  /// See also \a loadDataHandle(), which will also load first-class objects
  /// that only store data.
  virtual Optional<RawDataHandle> getRawData(Reference Ref) const = 0;

  /// Load data for \p Ref, treating it as-if \a ReferenceKind::RawData even if
  /// it's a first-class object.
  ///
  /// Returns \a DataHandle if \p Ref is \a ReferenceKind::RawData, a leaf \a
  /// ReferenceKind::Node, or \a ReferenceKind::Blob.
  ///
  /// Else, \p Ref must be a non-leaf \a ReferenceKind::Node or a \a
  /// ReferenceKind::Tree, and returns None.
  ///
  /// Errors if the object cannot be loaded.
  Expected<Optional<AnyDataHandle>> loadDataHandle(Reference Ref);

  /// Load \p Ref, and return it if it's an object.
  ///
  /// Returns \c None if it's not a first-class object.
  ///
  /// Errors if the object cannot be loaded.
  Expected<Optional<AnyObjectHandle>> loadObject(Reference Ref);

  /// Load the object called \p ID.
  ///
  /// Returns \c None if it's unknown in this CAS instance.
  ///
  /// Errors if the object cannot be loaded.
  Expected<Optional<AnyObjectHandle>> loadObject(const CASID &ID);

  /// If \p Ref is \a ReferenceKind::Object, load and return an \a
  /// ObjectHandle.
  ///
  /// Else, \a ReferenceKind::RawData. Create and return an \a ObjectHandle
  /// for an object with the same data.
  ///
  /// NOTE: It feels like this should be used sparingly, if ever. Instead,
  /// schemas should create objects up-front for any references that need
  /// a \a CASID.
  Expected<AnyObjectHandle> getOrCreateObject(Reference Ref);

protected:
  /// Create an object for raw data.
  ///
  /// Only need to override if overriding \a storeDataImpl().
  virtual Expected<AnyObjectHandle> createObjectFromRawData(RawDataHandle H) {
    llvm_unreachable("not implemented");
  }

private:
  static Error createUnknownObjectError(CASID ID);
  static Error createWrongKindError(CASID ID);

  template <class ProxyT, class HandleT>
  Expected<ProxyT> loadObjectProxy(CASID ID);

  template <class ProxyT, class HandleT>
  Expected<ProxyT> loadObjectProxy(Expected<HandleT> H);

public:
  /// FIXME: Delete these. Update callers to call \a loadObject(), followed by
  /// \a getBlob(), etc., or \a BlobProxy::get().
  Expected<BlobProxy> getBlob(CASID ID);
  Expected<TreeProxy> getTree(CASID ID);
  Expected<NodeProxy> getNode(CASID ID);

  /// Get the size of some data.
  virtual uint64_t getDataSize(AnyDataHandle Data) const = 0;

  /// Read the data from \p Data into \p OS.
  uint64_t readData(AnyDataHandle Data, raw_ostream &OS, uint64_t Offset = 0,
                    uint64_t MaxBytes = -1ULL) const {
    return readDataImpl(Data, OS, Offset, MaxBytes);
  }

protected:
  virtual uint64_t readDataImpl(AnyDataHandle Data, raw_ostream &OS,
                                uint64_t Offset, uint64_t MaxBytes) const = 0;

public:
  /// Get a lifetime-extended StringRef pointing at \p Data.
  ///
  /// Depending on the CAS implementation, this may involve in-memory storage
  /// overhead.
  StringRef getDataString(AnyDataHandle Data, bool NullTerminate = true) {
    return toStringRef(getDataImpl(Data, NullTerminate));
  }

  /// Get a lifetime-extended ArrayRef pointing at \p Data.
  ///
  /// Depending on the CAS implementation, this may involve in-memory storage
  /// overhead.
  template <class CharT = char>
  ArrayRef<CharT> getDataArray(AnyDataHandle Data, bool NullTerminate = true) {
    static_assert(std::is_same<CharT, char>::value ||
                      std::is_same<CharT, unsigned char>::value ||
                      std::is_same<CharT, signed char>::value,
                  "Expected byte type");
    ArrayRef<char> S = getDataImpl(Data, NullTerminate);
    return makeArrayRef(reinterpret_cast<const CharT *>(S.data()), S.size());
  }

protected:
  virtual ArrayRef<char> getDataImpl(AnyDataHandle Data,
                                     bool NullTerminate) = 0;

public:
  /// Get a MemoryBuffer with the contents of \p Data whose lifetime is
  /// independent of this CAS instance.
  Expected<std::unique_ptr<MemoryBuffer>>
  loadIndependentDataBuffer(AnyDataHandle Data, const Twine &Name = "",
                            bool NullTerminate = true) const;

protected:
  virtual Expected<std::unique_ptr<MemoryBuffer>>
  loadIndependentDataBufferImpl(AnyDataHandle Data, const Twine &Name,
                                bool NullTerminate) const;

public:
  virtual Error forEachRef(NodeHandle Node,
                           function_ref<Error(Reference)> Callback) const = 0;
  virtual void readRefs(NodeHandle Node, SmallVectorImpl<Reference> &Refs) const;
  virtual Reference readRef(NodeHandle Node, size_t I) const = 0;
  virtual size_t getNumRefs(NodeHandle Node) const = 0;

  virtual Error forEachTreeEntry(
      TreeHandle Tree,
      function_ref<Error(const NamedTreeEntry &)> Callback) const = 0;
  virtual NamedTreeEntry loadTreeEntry(TreeHandle Tree, size_t I) const = 0;
  virtual Optional<size_t> lookupTreeEntry(TreeHandle Tree,
                                           StringRef Name) const = 0;
  virtual size_t getNumTreeEntries(TreeHandle Tree) const = 0;

  virtual Expected<CASID> getCachedResult(CASID InputID) = 0;
  virtual Error putCachedResult(CASID InputID, CASID OutputID) = 0;

  virtual void print(raw_ostream &) const {}
  void dump() const;

  virtual ~CASDB() = default;
};

template <class HandleT> class ProxyBase : public HandleT {
public:
  const CASDB &getCAS() const { return *CAS; }
  CASID getID() const {
    return CAS->getObjectID(*static_cast<const ObjectHandle *>(this));
  }

  /// FIXME: Remove this.
  operator CASID() const { return getID(); }

protected:
  ProxyBase(const CASDB &CAS, HandleT H) : HandleT(H), CAS(&CAS) {}
  const CASDB *CAS;
};

/// Proxy for a Blob.
class BlobProxy : public ProxyBase<BlobHandle> {
public:
  /// Get the content of the blob. Valid as long as the CAS is valid.
  StringRef getData() const { return Data; }
  ArrayRef<char> getDataArray() const {
    return makeArrayRef(Data.begin(), Data.size());
  }
  StringRef operator*() const { return Data; }
  const StringRef *operator->() const { return &Data; }

  static BlobProxy load(CASDB &CAS, BlobHandle Blob) {
    return BlobProxy(CAS, Blob, CAS.getDataString(Blob.getData()));
  }

private:
  BlobProxy(const CASDB &CAS, BlobHandle H, StringRef Data)
      : ProxyBase::ProxyBase(CAS, H), Data(Data) {
    assert(Data.end()[0] == 0 && "Blobs should guarantee null-termination");
  }

  StringRef Data;
};

/// Proxy of a tree CAS object. Reference is passed by value and is
/// expected to be valid as long as the \a CASDB is.
///
/// TODO: Add an API to expose a range of NamedTreeEntry.
///
/// FIXME: Turn into a reader API.
class TreeProxy : public ProxyBase<TreeHandle> {
public:
  bool empty() const { return NumEntries == 0; }
  size_t size() const { return NumEntries; }

  Optional<NamedTreeEntry> lookup(StringRef Name) const {
    if (Optional<size_t> I = getCAS().lookupTreeEntry(
            *static_cast<const TreeHandle *>(this), Name))
      return get(*I);
    return None;
  }

  NamedTreeEntry get(size_t I) const {
    return getCAS().loadTreeEntry(*static_cast<const TreeHandle *>(this), I);
  }

  /// Visit each tree entry in order, returning an error from \p Callback to
  /// stop early.
  Error
  forEachEntry(function_ref<Error(const NamedTreeEntry &)> Callback) const {
    return getCAS().forEachTreeEntry(*static_cast<const TreeHandle *>(this),
                                     Callback);
  }

  TreeProxy() = delete;

  static TreeProxy load(CASDB &CAS, TreeHandle Tree) {
    return TreeProxy(CAS, Tree, CAS.getNumTreeEntries(Tree));
  }

private:
  TreeProxy(const CASDB &CAS, TreeHandle H, size_t NumEntries)
      : ProxyBase::ProxyBase(CAS, H), NumEntries(NumEntries) {}

  size_t NumEntries;
};

/// Reference to an abstract hierarchical node, with data and references.
/// Reference is passed by value and is expected to be valid as long as the \a
/// CASDB is.
class NodeProxy : public ProxyBase<NodeHandle> {
public:
  size_t getNumReferences() const { return NumReferences; }
  Reference getReference(size_t I) const {
    return getCAS().readRef(*static_cast<const NodeHandle *>(this), I);
  }

  // FIXME: Remove this.
  CASID getReferenceID(size_t I) const {
    Optional<CASID> ID = getCAS().getObjectID(getReference(I));
    assert(ID && "Expected reference to be first-class object");
    return *ID;
  }

  /// Visit each reference in order, returning an error from \p Callback to
  /// stop early.
  Error forEachReference(function_ref<Error(Reference)> Callback) const {
    return getCAS().forEachRef(*static_cast<const NodeHandle *>(this),
                               Callback);
  }
  Error forEachReferenceID(function_ref<Error(CASID)> Callback) const {
    return getCAS().forEachRef(
        *static_cast<const NodeHandle *>(this), [&](Reference Ref) {
          Optional<CASID> ID = getCAS().getObjectID(Ref);
          assert(ID && "Expected reference to be first-class object");
          return Callback(*ID);
        });
  }

  /// Get the content of the node. Valid as long as the CAS is valid.
  StringRef getData() const { return Data; }

  NodeProxy() = delete;

  static NodeProxy load(CASDB &CAS, NodeHandle Node) {
    return NodeProxy(CAS, Node, CAS.getNumRefs(Node),
                     CAS.getDataString(Node.getData()));
  }

private:
  NodeProxy(const CASDB &CAS, NodeHandle H, size_t NumReferences,
            StringRef Data)
      : ProxyBase::ProxyBase(CAS, H), NumReferences(NumReferences), Data(Data) {
  }

  size_t NumReferences;
  StringRef Data;
};

Expected<std::unique_ptr<CASDB>>
createPluginCAS(StringRef PluginPath, ArrayRef<std::string> PluginArgs = None);
std::unique_ptr<CASDB> createInMemoryCAS();

/// Gets or creates a persistent on-disk path at \p Path.
///
/// Deprecated: if \p Path resolves to \a getDefaultOnDiskCASStableID(),
/// automatically opens \a getDefaultOnDiskCASPath() instead.
///
/// FIXME: Remove the special behaviour for getDefaultOnDiskCASStableID(). The
/// client should handle this logic, if/when desired.
Expected<std::unique_ptr<CASDB>> createOnDiskCAS(const Twine &Path);

/// Set \p Path to a reasonable default on-disk path for a persistent CAS for
/// the current user.
void getDefaultOnDiskCASPath(SmallVectorImpl<char> &Path);

/// Get a reasonable default on-disk path for a persistent CAS for the current
/// user.
std::string getDefaultOnDiskCASPath();

/// FIXME: Remove.
void getDefaultOnDiskCASStableID(SmallVectorImpl<char> &Path);

/// FIXME: Remove.
std::string getDefaultOnDiskCASStableID();

} // namespace cas
} // namespace llvm

#endif // LLVM_CAS_CASDB_H
