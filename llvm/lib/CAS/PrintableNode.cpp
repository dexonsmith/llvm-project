//===- CAS/PrintableNode.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/PrintableNode.h"
#include "llvm/CAS/CASDB.h"

using namespace llvm;
using namespace llvm::cas;

char PrintableNodeSchema::ID = 0;
void PrintableNodeSchema::anchor() {}

Expected<ObjectRef> PrintableNodeSchema::createTypeID(CASDB &CAS) {
  // FIXME: Just the name isn't really good enough. We should encode the enum
  // values as well. E.g.:
  //
  //       SmallVector<1024> ID = "llvm:printable-node:...";
  //     #define NODE_FIELD_KIND(NAME)                                         \
  //       ((":" #NAME "=" + Twine((uint8_t)FieldKind::NAME)).toVector(ID);
  //     #include "FieldKind.def"
  //     #define NODE_STORAGE_CLASS(NAME)                                      \
  //       ((":" #NAME "=" + Twine((uint8_t)FieldValue::StorageClass::NAME))   \
  //           .toVector(ID);
  //     #include "FieldValue.def"
  //
  // Or something similar that incorporates this information. Maybe the string
  // could even be precomputed at compile-time as a string-literal.
  constexpr StringLiteral PrintableNodeTypeID = "llvm:printable-node:root:v0";
  if (Expected<NodeHandle> Node = CAS.storeNode(None, PrintableNodeTypeName))
    return CAS.getReference(*Node);
  else
    return Node.takeError();
}

bool PrintableNodeSchema::isRootNode(const NodeHandle &Node) const {
  if (CAS.getNumRefs() < 1)
    return false;
  return CAS.readRef(Node, 0) == *TypeID;
}

Expected<PrintableNodeSchema> PrintableNodeSchema::get(CASDB &CAS) {
  if (Expected<ObjectRef> TypeID = createTypeID(CAS))
    return PrintableNodeSchema(CAS, TypeID);
  else
    return TypeID.takeError();
}

void PrintableNodeBuilder::addObject(ObjectHandle Handle,
                                     Optional<StringRef> Name) {
  return addObject(CAS.getReference(Handle), Name);
}

void PrintableNodeBuilder::addPrintableString(StringRef String,
                                              Optional<StringRef> Name) {
  auto Kind = FieldKind::PrintableString;
  if (String.size() <= 16U)
    saveField(FieldValue(Kind, saveString(String)), Name);
  else
    outlineField(Kind, arrayRefFromStringRef<char>(String), Name);
}

void PrintableNodeBuilder::addOpaqueData(ArrayRef<char> Data,
                                         Optional<StringRef> Name) {
  auto Kind = FieldKind::OpaqueData;
  if (Data.size() <= 16U)
    saveField(FieldValue(Kind, saveString(toStringRef(Data))), Name);
  else
    outlineField(Kind, Data, Name);
}

void PrintableNodeBuilder::addSigned64(int64_t N, Optional<StringRef> Name) {
  saveField(FieldValue(FieldKind::Signed64, N), Name);
}
void PrintableNodeBuilder::addUnsigned64(int64_t N, Optional<StringRef> Name);

StringMapEntry<StringInfo> &PrintableNodeBuilder::saveString(StringRef S) {
  auto Insertion = StringsLookup.insert({S, -1U});
  auto &Entry = *Insertion.first;
  if (Insertion.second)
    Strings.push_back(&Entry);
  return &Entry;
}

void PrintableNodeBuilder::saveField(FieldValue &&V, Optional<StringRef> Name) {
  assert((!Name || Name->size() <= UINT16_MAX) &&
         "Expected name to be much smaller");
  auto *Field = new (FieldAlloc.Allocate()) FieldValue(std::move(V));
  if (Name)
    NamedFieldsMap[saveString(*Name)] = Field;
  else
    UnnamedFields.push_back(Field);
}

Expected<NodeHandle> PrintableNodeBuilder::build() {
  if (HasBuilt)
    report_fatal_error("PrintableNodeBuilder already built");
  HasBuilt = true;

  if (DelayedError)
    return std::move(DelayedError);

  // Sort strings and assign numbers.
  llvm::sort(Strings, [](StringMapEntry<StringInfo> *LHS,
                         StringMapEntry<StringInfo> *RHS) {
    return LHS->first() < RHS->first();
  });
  if (Strings.size() > UINT16_MAX)
    return createStringError(inconvertibleErrorCode(),
                             "Too many strings in NodeBuilder");
  size_t NextOffset = 0;
  using StringSizeT = uint16_t;
  for (size_t I = 0, E = Strings.size(); I != E; ++I) {
    StringRef String = Strings[I]->first();
    assert((StringSizeT)String.size() == String.size());

    StringInfo &Info = Strings[I]->second;
    Info.Offset = NextOffset;
    NextOffset += alignTo(AlignOf<StringSizeT>(), String.size() + 1);
  }
  using NamedField = decltype(*NamedFieldsMap.begin());
  SmallVector<NamedField *> NamedFields;

  // Collect the refs, starting with the RootTypeID for PrintableNodeSchema.
  SmallVector<ObjectRef> Refs = {Schema.getRootTypeID()};

  auto writeString = [](raw_ostream &OS, StringRef S) {
    assert((StringSizeT)S.size() == S.size());
    endian::write(OS, (StringSizeT)S.size(), endianness::little);
    OS << S;
    OS.write('\0');
  };

  // Create the manifest, which describes the layout.
  SmallString<256> LayoutManifest;
  size_t NumRefFields = 0;
  size_t NumStringFields = 0;
  size_t NumNumber32Fields = 0;
  size_t NumNumber64Fields = 0;
  using llvm::support;
  using llvm::support::endian::endianness;
  {
    // Layout of the manifest:
    //
    // 1. Counts of unnamed vs. named fields.
    // - num-unnamed-fields: 4B
    // - num-named-fields:   4B
    //
    // 2. Counts of values stored by ref vs. number vs. string.
    // - num-ref-fields:      4B
    // - num-string-fields:   4B
    // - num-number32-fields: 4B
    // - num-number64-fields: 4B
    //
    // 3. Field descriptions; first unnamed, then named in sorted order:
    // - kind:    1B
    // - storage: 1B
    //
    // 4. Node name.
    // - size            : 2B
    // - Bytes           : 0B+
    // - null-terminator : 1B
    auto describeField = [&](const FieldValue &FV) {
      OS.write((uint8_t)FV.Kind);
      OS.write((uint8_t)FV.Storage);
      NumRefFields += FV.Storage == FieldValue::StorageClass::Ref;
      NumStringFields += FV.Storage == FieldValue::StorageClass::String;
      NumNumber32Fields += FV.Storage == FieldValue::StorageClass::Number32;
      NumNumber64Fields += FV.Storage == FieldValue::StorageClass::Number64;
    };
    auto reserveSlot = [&]() {
      uint32_t Offset = LayoutManifest.size();
      LayoutManifest.resize(LayoutManifest.size() + sizeof(uint32_t));
      return Offset;
    };
    raw_svector_ostream OS(LayoutManifest);
    endian::write(OS, (uint32_t)UnnamedFields.size(), endianness::little);
    endian::write(OS, (uint32_t)NamedFields.size(), endianness::little);
    const uint32_t RefsSlot = reserveSlot();
    const uint32_t StringsSlot = reserveSlot();
    const uint32_t Numbers32Slot = reserveSlot();
    const uint32_t Numbers64Slot = reserveSlot();
    for (const FieldValue *FV : UnnamedFields)
      describeField(*FV);
    for (StringMapEntry<StringInfo> *S : Strings) {
      auto I = NamedFieldsMap.find(S);
      if (I == NamedFieldsMap.end())
        continue;
      NamedFields.push_back(&*I);
      describeField(*I->second);
    }
    assert(NamedFields.size() == NamedFieldsMap.size());
    assert(NumRefFields + NumStringFields + NumNumber32Fields +
               NumNumber64Fields ==
           UnnamedFields.size() + NamedFields.size());
    endian::write(&LayoutManifest[RefsSlot], NumRefFields, endianness::little);
    endian::write(&LayoutManifest[StringsSlot], NumStringFields,
                  endianness::little);
    endian::write(&LayoutManifest[Numbers32Slot], NumNumber32Fields,
                  endianness::little);
    endian::write(&LayoutManifest[Numbers64Slot], NumNumber64Fields,
                  endianness::little);

    if (KindName)
      writeString(*KindName);
  }
  {
    Optional<ObjectRef> ManifestID;
    if (Error E = CAS.storeNode(None, LayoutManifest).moveInto(ManifestID))
      return std::move(E);
    Refs.push_back(*ManifestID);
  }

  SmallString<256> Data;
  {
    // Layout of the data:
    //
    // 1. Field content (first unnamed, then named).
    // - Content : 4B {RefIndex,StringOffset,Number32,Number64Index}
    //
    // 2. String offsets for names of named fields.
    // - Name : 4B StringOffset
    //
    // 3. Padding to 8B-aligned.
    //
    // 4. 64-bit numbers.
    // - Value : 8B
    //
    // 5. Strings.
    // - Size                : 2B
    // - Bytes               : 0B+
    // - null-terminator     : 1B
    // - 0-pad to 2B-aligned : 1B?
    raw_svector_ostream OS(Data);

    // Field content.
    {
      size_t NextNumber64 = 0;
      auto writeFieldContent = [&](const FieldValue &FV) {
        uint32_t Content = -1U;
        switch (FV.Storage) {
        case FieldValue::StorageClass::Ref:
          Content = Refs.size();
          Refs.push_back(*FV.Ref);
          break;
        case FieldValue::StorageClass::String:
          Content = FV.String->Offset;
          break;
        case FieldValue::StorageClass::Number32:
          Content = *FV.Number;
          break;
        case FieldValue::StorageClass::Number32:
          Content = NextNumber64++;
          break;
        }
        endian::write(OS, Content, endianness::little);
        assert(isAligned(Align(4), Data.size()));
      };
      for (const FieldValue *FV : UnnamedFields)
        writeFieldContent(*FV);
      for (const NamedField *NF : NamedFields)
        writeFieldContent(NF->second);
      assert(isAligned(Align(4), Data.size()));
    }

    // String offsets for names of named fields.
    for (const NamedField *NF : NamedFields)
      endian::write(OS, NF->first->second.Offset, endianness::little);
    assert(isAligned(Align(4), Data.size()));

    // Pad to 8B-aligned.
    Data.resize(alignTo(Align(8), Data.size()));

    // 64-bit numbers.
    if (NumNumber64Fields) {
      auto writeIfNumber64 = [&OS](const FieldValue &FV) {
        if (NF->second.Storage == FieldValue::StorageClass::Number64)
          endian::write(OS, *FV.Number, endianness::little);
      };
      for (const FieldValue *FV : UnnamedFields)
        writeIfNumber64(FV);
      for (const NamedField *NF : NamedFields)
        writeIfNumber64(NF->second);
    }

    // Strings.
    if (NumStringFields) {
      auto writeIfString = [&OS](const FieldValue &FV) {
        if (FV.Storage != FieldValue::StorageClass::String)
          return;
        writeString(FV.String->first());
        while (!isAligned(AlignOf<StringSizeT>(), Data.size()))
          OS.write('\0');
      };
      for (const FieldValue *FV : UnnamedFields)
        writeIfString(*FV);
      for (const NamedField *NF : NamedFields)
        writeIfString(*NF->second);
    }
  }

  assert(Refs.size() == 2 + NumRefFields);
  return CAS.storeNode(Refs, Data);
}

void PrintableNodeBuilder::outlineField(FieldKind Kind, ArrayRef<char> Data,
                                        Optional<StringRef> Name) {
  if (Expected<NodeHandle> Node = getCAS().storeNode(None, Data))
    saveField(FieldValue(Kind, getCAS().getReference(*Node)), Name);
  else
    DelayedError = joinErrors(std::move(DelayedError), Node.takeError());
}

Optional<StringRef>
ObjectFileSchema::getKindString(const cas::NodeProxy &Node) const {
  assert(&Node.getCAS() == &CAS);
  StringRef Data = Node.getData();
  if (Data.empty())
    return None;

  unsigned char ID = Data[0];
  for (auto &I : KindStrings)
    if (I.first == ID)
      return I.second;
  return None;
}

bool ObjectFileSchema::isRootNode(const cas::NodeProxy &Node) const {
  if (Node.getNumReferences() < 1)
    return false;
  return Node.getReferenceID(0) == *RootNodeTypeID;
}

bool ObjectFileSchema::isNode(const cas::NodeProxy &Node) const {
  // This is a very weak check!
  return bool(getKindString(Node));
}

Expected<ObjectFormatNodeProxy::Builder>
ObjectFormatNodeProxy::Builder::startRootNode(const ObjectFileSchema &Schema,
                                              StringRef KindString) {
  Builder B(Schema);
  B.IDs.push_back(Schema.getRootNodeTypeID());

  if (Error E = B.startNodeImpl(KindString))
    return std::move(E);
  return std::move(B);
}

Error ObjectFormatNodeProxy::Builder::startNodeImpl(StringRef KindString) {
  Optional<unsigned char> TypeID = Schema->getKindStringID(KindString);
  if (!TypeID)
    return createStringError(inconvertibleErrorCode(),
                             "invalid object format kind string: " +
                                 KindString);
  Data.push_back(*TypeID);
  return Error::success();
}

Expected<ObjectFormatNodeProxy::Builder>
ObjectFormatNodeProxy::Builder::startNode(const ObjectFileSchema &Schema,
                                          StringRef KindString) {
  Builder B(Schema);
  if (Error E = B.startNodeImpl(KindString))
    return std::move(E);
  return std::move(B);
}

Expected<ObjectFormatNodeProxy> ObjectFormatNodeProxy::Builder::build() {
  return ObjectFormatNodeProxy::get(*Schema, Schema->CAS.createNode(IDs, Data));
}

StringRef ObjectFormatNodeProxy::getKindString() const {
  Optional<StringRef> KS = getSchema().getKindString(*this);
  assert(KS && "Expected valid kind string");
  return *KS;
}

Optional<unsigned char>
ObjectFileSchema::getKindStringID(StringRef KindString) const {
  for (auto &I : KindStrings)
    if (I.second == KindString)
      return I.first;
  return None;
}

Expected<ObjectFormatNodeProxy>
ObjectFormatNodeProxy::get(const ObjectFileSchema &Schema,
                           Expected<cas::NodeProxy> Ref) {
  if (!Ref)
    return Ref.takeError();
  if (!Schema.isNode(*Ref))
    return createStringError(
        inconvertibleErrorCode(),
        "invalid kind-string for node in object-file-schema");
  return ObjectFormatNodeProxy(Schema, *Ref);
}
