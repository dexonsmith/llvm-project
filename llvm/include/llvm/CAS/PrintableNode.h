//===- llvm/CAS/PrintableNode.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CAS_PRINTABLENODE_H
#define LLVM_CAS_PRINTABLENODE_H

#include "llvm/CAS/CASReference.h"
#include "llvm/CASObjectFormats/Data.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/Support/ExtensibleRTTI.h"

namespace llvm {
namespace cas {
namespace printable_nodes {

// TODO: Move to a '.defs' file so it's easy to iterate through these. See
// comment inside \a createTypeID().
enum class PNFieldKind {
  /// ObjectRef or ObjectHandle.
  Object,

  /// Opaque data.
  OpaqueData,

  /// Printable string.
  PrintableString,

  /// A unsigned number in 64-bits.
  Unsigned64,

  /// A signed number in 64-bits.
  Signed64,

  /// Argument block (argv).
  ///
  /// TODO: Implement. Builder should accept ArrayRef<const char *> or
  /// ArrayRef<StringRef> as arguments, encode the block, and return. Perhaps
  /// also
  /// \a StringRef that in some other known encoded form. Reader can fill a
  /// SmallVectorImpl with const char*.
  ///
  /// Encoding might be something like:
  /// - argc    : 4B
  /// - argv[]  : 4B offsets into the block of strings
  /// - strings block, with back-to-back:
  ///     - arg  : 0B+
  ///     - '\0' : 1B
  ///
  /// Or, arguments can be stored as separate refs.
  ArgumentBlock,

  /// Environment block (envp).
  ///
  /// TODO: Implement. See \a ArgumentBlock for inspiration.
  EnvironmentBlock,
};

class PNSchema final : public RTTIExtends<PNSchema, NodeSchema> {
  void anchor() override;

public:
  static char ID;

  /// Check if \a Node is a root (entry node) for the schema. This is a strong
  /// check, since it requires that the first reference matches a complete
  /// type-id DAG.
  bool isRootNode(const cas::NodeHandle &Node) const final;

  static Expected<cas::ObjectRef> createTypeID(cas::CASDB &CAS);
  static Expected<PNSchema> get(cas::CASDB &CAS);

private:
  PNSchema(CASDB &CAS, ObjectRef TypeID) : NodeSchema(CAS), TypeID(TypeID) {}
  ObjectRef TypeID;
};

/// Printable node builder.
///
/// Example usage:
///
/// ```
/// SchemaPool &Pool;
/// ArrayRef<uint32_t> HashForTableGenExec;
/// ArrayRef<const char *> CommandLine;
/// ArrayRef<ObjectRef> InputFile;
/// ArrayRef<TreeHandle> IncludeTree;
///
/// auto Builder = PNBuilder::start(
///                    Pool, "llvm::tablegen::main-job");
/// Builder.addOpaqueData(HashForTableGenExec, "executable");
/// Builder.addArgumentBlock(CommandLine, "command-line");
/// Builder.addObject(InputFile, "input");
/// Builder.addObject(IncludeTree, "includes");
/// ```
///
/// Or, if \a addArgumentBlock() hasn't been implemented (yet), that could be
/// \a addOpaqueData().
class PNBuilder {
  struct FieldValue;

public:
  void addObject(ObjectHandle Handle, Optional<StringRef> Name = None);
  void addObject(ObjectRef Ref, Optional<StringRef> Name = None) {
    saveFieldValue(FieldValue(PNFieldKind::Object, Ref), Name);
  }

  void addOpaqueData(ArrayRef<char> Data, Optional<StringRef> Name = None);
  void addOpaqueData(StringRef Data, Optional<StringRef> Name = None) {
    return addOpaqueData(arrayRefFromStringRef<char>(Data), Name);
  }
  void addOpaqueData(ArrayRef<uint8_t> String,
                     Optional<StringRef> Name = None) {
    return addOpaqueData(toStringRef(String, Name));
  }

  void addPrintableString(StringRef String, Optional<StringRef> Name = None);

  void addSigned64(int64_t N, Optional<StringRef> Name = None) {
    saveField(FieldValue(PNFieldKind::Signed64, N), Name);
  }
  void addUnsigned64(int64_t N, Optional<StringRef> Name = None) {
    saveField(FieldValue(PNFieldKind::Unsigned64, N), Name);
  }
  template <class T, std::enable_if_t<std::is_integral<T>::value, bool> = false>
  void addInteger(T N, Optional<StringRef> Name = None) {
    if (std::numeric_limits<T>::is_signed)
      addSigned64(N, Name);
    else
      addUnsigned64(N, Name);
  }

  /// Build the node.
  ///
  /// Returns Error if one of the builder functions triggered a delayed error,
  /// or if the final node cannot be built.
  ///
  /// Crashes if called twice.
  Expected<NodeHandle> build();

  static PNBuilder start(SchemaPool &Schemas,
                         Optional<StringRef> KindName = None);
  static PNBuilder start(SchemaPool &Schemas,
                         Optional<StringRef> KindName = None);

  /// If \a build() was never called, consume any delayed error.
  ~PNBuilder() { consumeError(std::move(DelayedError)); }

private:
  PNBuilder(PNSchema &Schema, Optional<StringRef> KindName)
      : CAS(CAS), Strings(StringsAlloc) {
    if (!KindName)
      return;
    assert(KindName->size() <= UINT16_MAX &&
           "Kind names should be less than 64K");
    this->KindName = KindName.str();
  }
  struct StringInfo {
    uint32_t Offset = -1U;
  };
  struct FieldValue {
    // TODO: Move to a '.defs' file so it's easy to iterate through these. See
    // comment inside \a PNSchema::createTypeID().
    enum class StorageClass {
      Ref,
      String,
      Number32,
      Number64,
    };

    PNFieldKind Kind;
    StorageClass Storage;

    Optional<ObjectRef> Ref;
    StringMapEntry<StringInfo> *String = nullptr;
    Optional<uint64_t> Number;

    FieldValue(PNFieldKind Kind, ObjectRef Ref)
        : Kind(Kind), Storage(StorageClass::Ref), Ref(Ref) {}
    FieldValue(PNFieldKind Kind, StringMapEntry<StringInfo> &String)
        : Kind(Kind), Storage(StorageClass::String), String(&String) {}
    FieldValue(PNFieldKind Kind, uint64_t Number)
        : Kind(Kind), Storage(Number <= UINT32_MAX ? Number32 : Number64),
          Number(Number) {}
  };

  StringMapEntry<StringInfo> &saveString(StringRef S);
  void saveField(FieldValue &&V, Optional<StringRef> Name);
  void outlineField(PNFieldKind Kind, ArrayRef<char> Data,
                    Optional<StringRef> Name);

  CASDB &CAS;
  BumpPtrAllocator StringsAlloc;
  StringSet<BumpPtrAllocator> StringsLookup;
  SmallVector<StringMapEntry<StringInfo> *> Strings;
  SpecificBumpPtrAllocator<FieldValue> FieldAlloc;

  Optional<std::string> KindName;
  DenseSet<StringMapEntry<StringInfo> *, FieldValue *> NamedFieldsMap;
  SmallVector<FieldValue *> UnnamedFields;

  Error DelayedError = Error::success();
  bool HasBuilt = false;
};

class PNField {
public:
  PNFieldKind getKind() const { return Kind; }

protected:
  PNField(PNFieldKind Kind) : Kind(Kind) {}

private:
  PNFieldKind Kind;
};

class ObjectPNField : public PNField {
public:
  static bool classof(const PNField *F) {
    return getKind() == PNFieldKind::Object;
  }

  ObjectPNField(ObjectRef Ref) : Ref(Ref) {}
  ObjectRef Ref;
};

class OpaqueDataPNField : public PNField {
public:
  static bool classof(const PNField *F) {
    return getKind() == PNFieldKind::OpaqueData;
  }
  ObjectPNField(ArrayRef<char> Data) : Data(Data) {}
  ArrayRef<char> Data;
};

class PrintableStringPNField : public PNField {
public:
  static bool classof(const PNField *F) {
    return getKind() == PNFieldKind::PrintableString;
  }
  ObjectPNField(StringRef String) : String(String) {}
  StringRef String;
};

class Number64PNField : public PNField {
public:
  static bool classof(const PNField *F) {
    return getKind() == PNFieldKind::Unsigned64 ||
           getKind() == PNFieldKind::Signed64;
  }

protected:
  explicit Number64PNField(uint64_t RawNumber) : RawNumber(RawNumber) {}
  uint64_t RawNumber;
};

class Unsigned64PNField : public Number64PNField {
public:
  static bool classof(const PNField *F) {
    return getKind() == PNFieldKind::Unsigned64;
  }

  uint64_t get() const { return RawNumber; }

  template <class T, std::enable_if_t<std::is_integral<T>::value, bool> = false>
  Optional<T> getAs() const {
    const uint64_t U = get();
    if (U > std::numeric_limits<T>::max)
      return None;
    return U;
  }

  explicit Unsigned64PNField(uint64_t RawNumber) : Number64PNField(RawNumber) {}
};

class Signed64PNField : public Number64PNField {
public:
  static bool classof(const PNField *F) {
    return getKind() == PNFieldKind::Signed64;
  }

  int64_t get() const { return (int64_t)RawNumber; }

  template <class T, std::enable_if_t<std::is_integral<T>::value, bool> = false>
  Optional<T> getAs() const {
    const int64_t I = get();
    if (I >= 0) {
      if (I > std::numeric_limits<T>::max)
        return None;
      return I;
    }
    if (!std::numeric_limits<T>::is_signed || I < std::numeric_limits<T>::min)
      return None;
    return I;
  }

  explicit Signed64PNField(uint64_t RawNumber) : Number64PNField(RawNumber) {}
};

class ArgumentBlockPNField : public PNField {
public:
  static bool classof(const PNField *F) {
    return getKind() == PNFieldKind::ArgumentBlock;
  }
};

class EnvironmentBlockPNField : public PNField {
public:
  static bool classof(const PNField *F) {
    return getKind() == PNFieldKind::EnvironmentBlock;
  }
};

/// Pool of PNFields, with lifetime tied to the pool.
///
/// TODO: Consider deduplicating by content. E.g., there could be use cases
/// where the same pool is used for reading multiple handles.
class PNFieldPool {
public:
  PNFieldPool() : Schema(Schema) {}

  ObjectPNField &makeObject(ObjectRef Ref) {
    return new (Alloc) ObjectPNField(Ref);
  }
  OpaqueDataPNField &makeOpaqueData(ArrayRef<char> Data) {
    return new (Alloc) OpaqueDataPNField(Data);
  }
  PrintableStringPNField &makePrintableString(StringRef String) {
    return new (Alloc) PrintableStringPNField(String);
  }
  Unsigned64PNField &makeUnsigned64(uint64_t RawNumber) {
    return new (Alloc) Unsigned64PNField(RawNumber);
  }
  Signed64PNField &makeSigned64(uint64_t RawNumber) {
    return new (Alloc) Signed64PNField(RawNumber);
  }

  StringRef store(StringRef String) {
    StringSaver Saver(Alloc);
    return Saver.save(String);
  }
  ArrayRef<char> store(ArrayRef<char> Data) {
    return arrayRefFromStringRef<char>(store(toStringRef(Data)));
  }

private:
  BumpPtrAllocator Alloc;
};

/// Printable node reader.
///
/// Example usage:
///
/// ```
/// SchemaPool &Pool;
/// NodeHandle &Node;
/// Optional<PNReader> Reader;
/// if (Error E = PNReader::parse(Node).moveInto(Reader))
///   return E;
/// Reader->dump(); // Dump the full node.
/// Reader->get()
/// Reader->get("executable")->print();
/// Builder.addArgumentBlock(CommandLine, "command-line");
/// Builder.addObject(InputFile, "input");
/// Builder.addObject(IncludeTree, "includes");
/// ```
class PNReader {
public:
  void print(raw_ostream &OS) const;
  LLVM_DEBUG_METHOD void dump() const;

  Optional<StringRef> getKindName() const;
  size_t getNumUnnamedFields() const;
  size_t getNumNamedFields() const;

  const PNField *getNamedField(StringRef Name);
  const PNField *getUnnamedField(size_t UnnamedIndex);

  void print(raw_ostream &OS) const;
  LLVM_DUMP_METHOD void dump() const;

  Error forEachUnnamedField(function_ref<void(size_t Index, const PNField &F)>);
  Error forEachNamedField(function_ref<void(StringRef Name, const PNField &F)>);

  static Expected<PNReader> parse(PNSchema &Schema, ObjectRef Ref,
                                  PNFieldPool *Pool = nullptr);

  static Expected<PNReader> parse(PNSchema &Schema, NodeHandle Node,
                                  PNFieldPool *Pool = nullptr) {
    PNReader Reader(Schema, Node, Pool);
    if (Error E = Reader.parse())
      return std::move(E);
    return std::move(Reader);
  }

private:
  PNReader(PNSchema &Schema, NodeHandle Node, PNFieldPool *Pool)
      : Schema(&Schema), Node(Node), Pool(Pool) {
    if (Pool)
      return;
    OwnedPool.emplace();
    this->Pool = &*OwnedPool;
  }

  Error parse();

  Optional<PNFieldPool> OwnedPool;
  PNSchema *Schema;
  PNFieldPool *Pool;
  NodeHandle Node;

  SmallVector<const PNField *> UnnamedFields;
  SmallVector<std::pair<StringRef, const PNField *>> NamedFields;
};

} // end namespace printable_nodes
} // end namespace cas
} // end namespace llvm

#endif // LLVM_CAS_PRINTABLENODE_H
