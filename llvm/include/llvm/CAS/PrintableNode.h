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

class PrintableNodeSchema final : public RTTIExtends<PrintableNodeSchema, NodeSchema> {
  void anchor() override;

public:
  // TODO: Move to a '.defs' file so it's easy to iterate through these. See
  // comment inside \a createTypeID().
  enum class FieldKind {
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
    /// ArrayRef<StringRef> as arguments, encode the block, and return. Perhaps also
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

  static char ID;

  /// Check if \a Node is a root (entry node) for the schema. This is a strong
  /// check, since it requires that the first reference matches a complete
  /// type-id DAG.
  bool isRootNode(const cas::NodeHandle &Node) const final;

  static Expected<cas::ObjectRef> createTypeID(cas::CASDB &CAS);
  static Expected<PrintableNodeSchema> get(cas::CASDB &CAS);

private:
  PrintableNodeSchema(CASDB &CAS, ObjectRef TypeID) : NodeSchema(CAS), TypeID(TypeID) {}
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
/// auto Builder = PrintableNodeBuilder::start(
///                    Pool, "llvm::tablegen::main-job");
/// Builder.addOpaqueData(HashForTableGenExec, "executable");
/// Builder.addArgumentBlock(CommandLine, "command-line");
/// Builder.addObject(InputFile, "input");
/// Builder.addObject(IncludeTree, "includes");
/// ```
///
/// Or, if \a addArgumentBlock() hasn't been implemented (yet), that could be
/// \a addOpaqueData().
class PrintableNodeBuilder {
  struct FieldValue;

public:
  using FieldKind = PrintableNodeSchema::FieldKind;

  void addObject(ObjectHandle Handle, Optional<StringRef> Name = None);
  void addObject(ObjectRef Ref, Optional<StringRef> Name = None) {
    saveFieldValue(FieldValue(FieldKind::Object, Ref), Name);
  }

  void addOpaqueData(ArrayRef<char> Data, Optional<StringRef> Name = None);
  void addOpaqueData(StringRef Data, Optional<StringRef> Name = None) {
    return addOpaqueData(arrayRefFromStringRef<char>(Data), Name);
  }
  void addOpaqueData(ArrayRef<uint8_t> String, Optional<StringRef> Name = None) {
    return addOpaqueData(toStringRef(String, Name));
  }

  void addPrintableString(StringRef String, Optional<StringRef> Name = None);

  void addSigned64(int64_t N, Optional<StringRef> Name = None) {
    saveField(FieldValue(FieldKind::Signed64, N), Name);
  }
  void addUnsigned64(int64_t N, Optional<StringRef> Name = None) {
    saveField(FieldValue(FieldKind::Unsigned64, N), Name);
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

  static PrintableNodeBuilder start(SchemaPool &Schemas,
                                    Optional<StringRef> KindName = None);
  static PrintableNodeBuilder start(SchemaPool &Schemas,
                                    Optional<StringRef> KindName = None);

  /// If \a build() was never called, consume any delayed error.
  ~PrintableNodeBuilder() { consumeError(std::move(DelayedError)); }

private:
  PrintableNodeBuilder(PrintableNodeSchema &Schema,
                       Optional<StringRef> KindName)
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
    // comment inside \a PrintableNodeSchema::createTypeID().
    enum class StorageClass {
      Ref,
      String,
      Number32,
      Number64,
    };

    FieldKind Kind;
    StorageClass Storage;

    Optional<ObjectRef> Ref;
    StringMapEntry<StringInfo> *String = nullptr;
    Optional<uint64_t> Number;

    FieldValue(FieldKind Kind, ObjectRef Ref)
        : Kind(Kind), Storage(StorageClass::Ref), Ref(Ref) {}
    FieldValue(FieldKind Kind, StringMapEntry<StringInfo> &String)
        : Kind(Kind), Storage(StorageClass::String), String(&String) {}
    FieldValue(FieldKind Kind, uint64_t Number)
        : Kind(Kind), Storage(Number <= UINT32_MAX ? Number32 : Number64), Number(Number) {}
  };

  StringMapEntry<StringInfo> &saveString(StringRef S);
  void saveField(FieldValue &&V, Optional<StringRef> Name);
  void outlineField(FieldKind Kind, ArrayRef<char> Data, Optional<StringRef> Name);

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

class PrintableNodeReader {
public:
  using FieldKind = PrintableNodeSchema::FieldKind;

  void print(raw_ostream &OS) const;
  LLVM_DEBUG_METHOD void dump() const;

  class FieldDescription {
  public:
    /// Casts to requested integer type. Errors if not valid.
    template <class IntT> IntT getInteger();
    StringRef getPrintableString() const; // May load Ref to get string.
    ArrayRef<char> getOpaqueData() const; // May load Ref to get string.

    CASDB &CAS;
    FieldKind Kind;
    Optional<StringRef> Name;

  private:
    Optional<uint64_t> EncodedNumber;
    Optional<StringRef> InlineString;
    Optional<ObjectRef> Ref;
  };

  Optional<StringRef> getKindName() const;
  size_t getNumUnnamedFields() const;
  size_t getNumNamedFields() const;

  Expected<FieldDescription> getUnnamedField(size_t N);
  Expected<FieldDescription> getNamedField(StringRef Name);
  Error forEachField(function_ref<void (const FieldDescription &FD,
                                        Optional<StringRef> Name)>);

  static Expected<PrintableNodeReader>
  get(PrintableNodeSchema &Schema, ObjectRef Ref);
  static Expected<PrintableNodeReader>
  get(PrintableNodeSchema &Schema, NodeHandle Node) {
    PrintableNodeReader Reader(Schema, Node);
    if (Error E = Reader.parse())
      return std::move(E);
    return std::move(Reader);
  }

private:
  PrintableNodeBuilder(PrintableNodeSchema &Schema, NodeHandle Node) :
    Schema(&Schema), Node(Node){}

  Error parse();

  PrintableNodeSchema *Schema;
  NodeHandle Node;

  Optional<StringRef> KindName;
  StringRef LayoutManifest;
  uint64_t NamesOffset;
  uint64_t StringsOffset;
  // ...
};

} // namespace cas
} // namespace llvm

#endif // LLVM_CAS_PRINTABLENODE_H
