//===- llvm/CAS/CASReference.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CAS_CASREFERENCE_H
#define LLVM_CAS_CASREFERENCE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {

class raw_ostream;

namespace cas {

class CASDB;

namespace testing_helpers {
class HandleFactory;
}

/// Reference to any object or raw data in the CAS. The object may or may not
/// be loaded.
///
/// \a CASDB::storeNode() takes a list of these, and these are returned by \a
/// CASDB::forEachRef() and \a CASDB::readRef(), which are accessors for nodes,
/// and \a CASDB::getReference()
///
/// \a CASDB::load() will load the referenced object or data, and returns \a
/// AnyHandle, a variant that knows what kind of entity it is. \a
/// CASDB::getReferenceKind() can expect the type of reference without asking
/// for unloaded objects to be loaded.
/// and \a CASDB::getRawData(), which
///
/// This is a wrapper around a \c uint64_t (and a \a CASDB instance when
/// assertions are on). If necessary, it can be deconstructed and reconstructed
/// using \a Reference::getInternalRef() and \a
/// Reference::getFromInternalRef(), but clients aren't expected to need to do
/// this. These both require the right \a CASDB instance.
class Reference {
public:
  /// Get an internal reference.
  uint64_t getInternalRef(const CASDB &ExpectedCAS) const {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(CAS == &ExpectedCAS && "Extracting reference for the wrong CAS");
#endif
    return InternalRef;
  }

  /// Print internal ref and/or CASID. Only suitable for debugging.
  void print(raw_ostream &OS) const;

  LLVM_DUMP_METHOD void dump() const;

  /// Allow a reference to be recreated after it's deconstructed.
  static Reference getFromInternalRef(const CASDB &CAS, uint64_t InternalRef) {
    return Reference(&CAS, InternalRef);
  }

  friend bool operator==(const Reference &LHS, const Reference &RHS) {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(LHS.CAS == RHS.CAS && "Cannot compare across CAS instances");
#endif
    return LHS.InternalRef == RHS.InternalRef;
  }
  friend bool operator!=(const Reference &LHS, const Reference &RHS) {
    return !(LHS == RHS);
  }

private:
  friend class CASDB;
  friend class testing_helpers::HandleFactory;
  Reference(const CASDB *CAS, uint64_t InternalRef) : InternalRef(InternalRef) {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    this->CAS = CAS;
#endif
  }

public:
  enum HandleKind {
    TreeKind = 0x1,
    NodeKind = 0x2,
    BlobKind = 0x4,
    ObjectKind = 0x8,
    RawDataKind = 0x10,
    AnyDataKind = RawDataKind | NodeKind | BlobKind,
    AnyObjectKind = ObjectKind | TreeKind | NodeKind | BlobKind,
    AnyKind = AnyObjectKind | AnyDataKind,
  };

private:
  friend class AnyHandle;
  template <class HandleT, HandleKind> friend class AnyHandleImpl;
  struct AnyHandleTag {};
  Reference(AnyHandleTag, Reference Other) : Reference(Other) {}

  uint64_t InternalRef;

#if LLVM_ENABLE_ABI_BREAKING_CHECKS
  const CASDB *CAS;
#endif
};

/// Handle to a loaded object or raw data in \a CASDB.
class Handle : public Reference {
  friend class CASDB;
  friend class AnyHandle;
  friend class testing_helpers::HandleFactory;
  using Reference::Reference;
  Handle(Reference) = delete;
  template <class HandleT, HandleKind> friend class AnyHandleImpl;
  static constexpr HandleKind getHandleKind() { return AnyKind; }
};

/// A handle to an object in the CAS that has been loaded.
class ObjectHandle : public Handle {
  friend class CASDB;
  friend class AnyHandle;
  friend class testing_helpers::HandleFactory;
  using Handle::Handle;
  ObjectHandle(Handle) = delete;
  template <class HandleT, HandleKind> friend class AnyHandleImpl;
  static constexpr HandleKind getHandleKind() { return ObjectKind; }
};

class AnyDataHandle;

/// Handle to a loaded blob in \a CASDB.
class BlobHandle : public ObjectHandle {
public:
  inline AnyDataHandle getData() const;

private:
  friend class CASDB;
  friend class testing_helpers::HandleFactory;
  using ObjectHandle::ObjectHandle;
  BlobHandle(ObjectHandle) = delete;
  template <class HandleT, HandleKind> friend class AnyHandleImpl;
  static constexpr HandleKind getHandleKind() { return BlobKind; }
};

/// Handle to a loaded node in \a CASDB.
class NodeHandle : public ObjectHandle {
public:
  inline AnyDataHandle getData() const;

private:
  friend class CASDB;
  friend class testing_helpers::HandleFactory;
  using ObjectHandle::ObjectHandle;
  NodeHandle(ObjectHandle) = delete;
  template <class HandleT, HandleKind> friend class AnyHandleImpl;
  static constexpr HandleKind getHandleKind() { return NodeKind; }
};

/// Handle to a loaded tree in \a CASDB.
class TreeHandle : public ObjectHandle {
  friend class CASDB;
  friend class testing_helpers::HandleFactory;
  using ObjectHandle::ObjectHandle;
  TreeHandle(ObjectHandle) = delete;
  template <class HandleT, HandleKind> friend class AnyHandleImpl;
  static constexpr HandleKind getHandleKind() { return TreeKind; }
};

/// Reference to raw data in the CAS.
class RawDataHandle : public Handle {
private:
  friend class CASDB;
  friend class testing_helpers::HandleFactory;
  using Handle::Handle;
  RawDataHandle(Handle) = delete;
  template <class HandleT, HandleKind> friend class AnyHandleImpl;
  static constexpr HandleKind getHandleKind() { return RawDataKind; }
};

/// Type-safe variant between all non-variant subclasses of \a Handle.
/// Besides the types accepted by \a AnyObjectHandle, this could also be a \a
/// RawDataHandle or an unadorned \a Handle.
template <class HandleBaseT, Handle::HandleKind BaseKind>
class AnyHandleImpl : public HandleBaseT {
public:
  template <class HandleT,
            std::enable_if_t<std::is_base_of<HandleBaseT, HandleT>::value &&
                                 (HandleT::getHandleKind() & BaseKind),
                             bool> = false>
  AnyHandleImpl(HandleT H) : AnyHandleImpl(H, HandleT::getHandleKind()) {}

  template <class HandleT,
            std::enable_if_t<std::is_base_of<HandleBaseT, HandleT>::value &&
                                 (HandleT::getHandleKind() & BaseKind),
                             bool> = false>
  bool is() const {
    constexpr auto NewKind = HandleT::getHandleKind();
    if (Kind == NewKind)
      return true;
    // Check for NewKind as a base class of Kind. E.g., NodeKind < ObjectKind.
    return (Kind & NewKind) && Kind < NewKind;
  }
  template <class HandleT> HandleT get() const {
    assert(is<HandleT>() && "Expected kind to match");
    return HandleT(typename HandleBaseT::AnyHandleTag{}, *this);
  }
  template <class HandleT> Optional<HandleT> dyn_cast() const {
    if (!is<HandleT>())
      return None;
    return get<HandleT>();
  }

protected:
  using HandleKind = typename HandleBaseT::HandleKind;
  using HandleBaseT::HandleBaseT;
  AnyHandleImpl(HandleBaseT H, HandleKind Kind) : HandleBaseT(H), Kind(Kind) {}
  HandleKind Kind;
};

/// Type-safe variant between \a ObjectHandle, \a TreeHandle, \a NodeHandle,
/// and \a BlobHandle.
class AnyObjectHandle
    : public AnyHandleImpl<ObjectHandle, Handle::AnyObjectKind> {
  friend class AnyHandle;

public:
  using AnyHandleImpl::AnyHandleImpl;
};

/// Type-safe variant between \a NodeHandle, \a BlobHandle, and \a
/// RawDataHandle.
class AnyDataHandle : public AnyHandleImpl<Handle, Handle::AnyDataKind> {
  friend class AnyHandle;

public:
  using AnyHandleImpl::AnyHandleImpl;

private:
  friend class BlobHandle;
  friend class NodeHandle;
  AnyDataHandle(BlobHandle H) : AnyHandleImpl::AnyHandleImpl(H) {}
  AnyDataHandle(NodeHandle H) : AnyHandleImpl::AnyHandleImpl(H) {}
};

inline AnyDataHandle BlobHandle::getData() const {
  return AnyDataHandle(*this);
}
inline AnyDataHandle NodeHandle::getData() const {
  return AnyDataHandle(*this);
}

/// Type-safe variant between \a Handle, \a DataHandle, and \a AnyObjectHandle.
class AnyHandle : public AnyHandleImpl<Handle, Handle::AnyKind> {
public:
  using AnyHandleImpl::AnyHandleImpl::AnyHandleImpl;
  AnyHandle(AnyObjectHandle H) : AnyHandleImpl::AnyHandleImpl(H, H.Kind) {}

  Optional<AnyObjectHandle> getAnyObject() const {
    if (Kind & AnyObjectKind)
      return AnyObjectHandle(ObjectHandle(Handle::AnyHandleTag{}, *this), Kind);
    return None;
  }
  Optional<AnyDataHandle> getData() const {
    if (Kind & AnyDataKind)
      return AnyDataHandle(Handle(Handle::AnyHandleTag{}, *this), Kind);
    return None;
  }
};

/// Some compile-time tests for handles.
///
/// FIXME: Move to tests.
inline void testHandles(AnyHandle H, AnyObjectHandle OH, AnyDataHandle DH) {
  H.is<ObjectHandle>();
  H.is<TreeHandle>();
  H.is<BlobHandle>();
  H.is<NodeHandle>();
  H.is<RawDataHandle>();
  H.get<ObjectHandle>();
  H.get<TreeHandle>();
  H.get<BlobHandle>();
  H.get<NodeHandle>();
  H.get<RawDataHandle>();

  OH.is<ObjectHandle>();
  OH.is<TreeHandle>();
  OH.is<BlobHandle>();
  OH.is<NodeHandle>();
  OH.get<ObjectHandle>();
  OH.get<TreeHandle>();
  OH.get<BlobHandle>();
  OH.get<NodeHandle>();

  DH.is<RawDataHandle>();
  DH.is<BlobHandle>();
  DH.is<NodeHandle>();
  DH.get<RawDataHandle>();
  DH.get<BlobHandle>();
  DH.get<NodeHandle>();

  H = OH;
  H = DH;
  OH = *H.getAnyObject();
  DH = *H.getData();

  // Fails to compile, as it should!
  //
  // DH.is<ObjectHandle>();
  // OH.is<RawDataHandle>();
}

} // namespace cas
} // namespace llvm

#endif // LLVM_CAS_CASREFERENCE_H
