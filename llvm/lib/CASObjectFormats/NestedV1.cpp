//===- CASObjectFormats/NestedV1.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CASObjectFormats/NestedV1.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/CASObjectFormats/CASObjectReader.h"
#include "llvm/CASObjectFormats/Encoding.h"
#include "llvm/CASObjectFormats/ObjectFormatHelpers.h"
#include "llvm/ExecutionEngine/JITLink/MemoryFlags.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/Threading.h"

// FIXME: For cl::opt. Should thread through or delete.
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace llvm::casobjectformats;
using namespace llvm::casobjectformats::nestedv1;
using namespace llvm::casobjectformats::reader;

constexpr StringLiteral NameRef::KindString;
constexpr StringLiteral BlockDataRef::KindString;
constexpr StringLiteral TargetListRef::KindString;
constexpr StringLiteral SectionRef::KindString;
constexpr StringLiteral BlockRef::KindString;
constexpr StringLiteral SymbolRef::KindString;
constexpr StringLiteral SymbolTableRef::KindString;
constexpr StringLiteral NameListRef::KindString;
constexpr StringLiteral CompileUnitRef::KindString;

namespace {
class EncodedDataRef : public SpecificRef<EncodedDataRef> {
  using SpecificRefT = SpecificRef<EncodedDataRef>;
  friend class SpecificRef<EncodedDataRef>;

public:
  static constexpr StringLiteral KindString = "cas.o:encoded-data";

  StringRef getEncodedList() const { return getData(); }

  static Expected<EncodedDataRef>
  create(const ObjectFileSchema &Schema,
         function_ref<void(SmallVectorImpl<char> &)> Encode);
  static Expected<EncodedDataRef> get(Expected<ObjectFormatNodeProxy> Ref);
  static Expected<EncodedDataRef> get(const ObjectFileSchema &Schema,
                                      cas::CASID ID) {
    return get(Schema.getNode(ID));
  }

private:
  explicit EncodedDataRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};
} // end namespace

constexpr StringLiteral EncodedDataRef::KindString;

void ObjectFileSchema::anchor() {}

namespace {

class NestedV1ObjectReader;

class SymbolNodeRefBase {
public:
  virtual Expected<CASSymbol>
  materialize(const NestedV1ObjectReader &Reader) const = 0;

  virtual ~SymbolNodeRefBase() = default;
};

class ExternalSymbolNodeRef : public SymbolNodeRefBase {
public:
  NameRef SymbolName;
  jitlink::Linkage Linkage;

  ExternalSymbolNodeRef(NameRef SymbolName, jitlink::Linkage Linkage)
      : SymbolName(std::move(SymbolName)), Linkage(Linkage) {}

  Expected<CASSymbol>
  materialize(const NestedV1ObjectReader &Reader) const override;
};

class DefinedSymbolNodeRef : public SymbolNodeRefBase {
public:
  SymbolRef Symbol;
  Optional<CASSymbolRef> KeptAliveBySymbol;

  DefinedSymbolNodeRef(SymbolRef Symbol,
                       Optional<CASSymbolRef> KeptAliveBySymbol)
      : Symbol(std::move(Symbol)),
        KeptAliveBySymbol(std::move(KeptAliveBySymbol)) {}

  Expected<CASSymbol>
  materialize(const NestedV1ObjectReader &Reader) const override;
};

class BlockNodeRef {
public:
  Optional<CASSymbolRef> ForSymbol;
  BlockRef Block;
  Optional<CASSymbolRef> KeptAliveBySymbol;

  BlockNodeRef(Optional<CASSymbolRef> ForSymbol, BlockRef Block,
               Optional<CASSymbolRef> KeptAliveBySymbol)
      : ForSymbol(std::move(ForSymbol)), Block(std::move(Block)),
        KeptAliveBySymbol(std::move(KeptAliveBySymbol)) {}

  Expected<CASBlock> materialize(const NestedV1ObjectReader &Reader) const;
  Error
  materializeFixups(const NestedV1ObjectReader &Reader,
                    function_ref<Error(const CASBlockFixup &)> Callback) const;
};

class SectionNodeRef {
public:
  SectionRef Section;

  SectionNodeRef(SectionRef Section) : Section(std::move(Section)) {}

  Expected<CASSection> materialize() const;
};

/// FIXME: \p forEachSymbol() visits only top-level symbols, it doesn't visit
/// all the symbols that have been encoded.
class NestedV1ObjectReader final : public CASObjectReader {
public:
  explicit NestedV1ObjectReader(CompileUnitRef CURef)
      : CURef(std::move(CURef)) {}

  Triple getTargetTriple() const override { return CURef.getTargetTriple(); }

  Error forEachSymbol(
      function_ref<Error(CASSymbolRef, CASSymbol)> Callback) const override;

  Expected<CASSection> materialize(CASSectionRef Ref) const override {
    auto NodeRef = static_cast<SectionNodeRef *>((void *)Ref.Idx);
    return NodeRef->materialize();
  }

  Expected<CASBlock> materialize(CASBlockRef Ref) const override {
    auto NodeRef = static_cast<BlockNodeRef *>((void *)Ref.Idx);
    return NodeRef->materialize(*this);
  }

  Expected<CASSymbol> materialize(CASSymbolRef Ref) const override {
    auto NodeRef = static_cast<SymbolNodeRefBase *>((void *)Ref.Idx);
    return NodeRef->materialize(*this);
  }

  Error materializeFixups(
      CASBlockRef Ref,
      function_ref<Error(const CASBlockFixup &)> Callback) const override {
    auto NodeRef = static_cast<BlockNodeRef *>((void *)Ref.Idx);
    return NodeRef->materializeFixups(*this, std::move(Callback));
  }

  template <class RefT> struct TypedID {
    cas::CASID ID;
    operator cas::CASID() const { return ID; }
    TypedID(RefT Ref) : ID(Ref.getID()) {}

  private:
    friend struct DenseMapInfo<TypedID>;
    explicit TypedID(cas::CASID ID) : ID(ID) {}
  };

  using TargetID = TypedID<TargetRef>;
  using SectionID = TypedID<SectionRef>;

  Expected<CASSymbolRef>
  getOrCreateCASSymbolRef(Expected<SymbolRef> Symbol,
                          Optional<CASSymbolRef> KeptAliveBySymbol) const;
  Expected<CASBlockRef> getOrCreateCASBlockRef(
      const DefinedSymbolNodeRef &ForSymbol, Expected<BlockRef> Block,
      Optional<CASSymbolRef> KeptAliveBySymbol, bool MergeByContent) const;
  Expected<CASSectionRef>
  getOrCreateCASSectionRef(Expected<SectionRef> Section) const;

  Optional<CASSymbolRef> getCASSymbolRefByName(StringRef Name) const;

private:
  Error forEachExternalSymbol(
      Expected<NameListRef> ExternalSymbols, jitlink::Linkage Linkage,
      function_ref<Error(CASSymbolRef, CASSymbol)> Callback) const;

  Expected<CASSymbolRef>
  getOrCreateExternalCASSymbolRef(const ObjectFileSchema &Schema,
                                  Expected<NameRef> SymbolName,
                                  jitlink::Linkage Linkage) const;

  Error
  forEachSymbol(Expected<SymbolTableRef> Table,
                function_ref<Error(CASSymbolRef, CASSymbol)> Callback) const;

  CompileUnitRef CURef;

  mutable sys::Mutex SymbolsLock;
  mutable DenseMap<TargetID, std::unique_ptr<SymbolNodeRefBase>> Symbols;
  mutable DenseMap<StringRef, CASSymbolRef> SymbolsByName;

  mutable sys::Mutex BlocksLock;
  mutable DenseMap<cas::CASID, std::unique_ptr<BlockNodeRef>> Blocks;

  mutable sys::Mutex SectionsLock;
  mutable DenseMap<SectionID, std::unique_ptr<SectionNodeRef>> Sections;
};

} // anonymous namespace

Expected<cas::NodeProxy>
ObjectFileSchema::createFromLinkGraphImpl(const jitlink::LinkGraph &G,
                                          raw_ostream *DebugOS) const {
  return CompileUnitRef::create(*this, G, DebugOS);
}

Expected<std::unique_ptr<CASObjectReader>>
ObjectFileSchema::createObjectReader(cas::NodeProxy RootNode) const {
  if (!isRootNode(RootNode))
    return createStringError(inconvertibleErrorCode(), "invalid root node");
  auto CU = CompileUnitRef::get(*this, RootNode);
  if (!CU)
    return CU.takeError();
  return CU->createObjectReader();
}

ObjectFileSchema::ObjectFileSchema(cas::CASDB &CAS) : SchemaBase(CAS) {
  // Fill the cache immediately to preserve thread-safety.
  if (Error E = fillCache())
    report_fatal_error(std::move(E));
}

Error ObjectFileSchema::fillCache() {
  Optional<cas::CASID> RootKindID;
  const unsigned Version = 0; // Bump this to error on old object files.
  if (Expected<cas::NodeProxy> ExpectedRootKind =
          CAS.createNode(None, "cas.o:nestedv1:schema:" + Twine(Version).str()))
    RootKindID = *ExpectedRootKind;
  else
    return ExpectedRootKind.takeError();

  StringRef AllKindStrings[] = {
      BlockDataRef::KindString,   BlockRef::KindString,
      CompileUnitRef::KindString, EncodedDataRef::KindString,
      NameListRef::KindString,    NameRef::KindString,
      SectionRef::KindString,     SymbolRef::KindString,
      SymbolTableRef::KindString, TargetListRef::KindString,
  };
  cas::CASID Refs[] = {*RootKindID};
  SmallVector<cas::CASID> IDs = {*RootKindID};
  for (StringRef KS : AllKindStrings) {
    auto ExpectedID = CAS.createNode(Refs, KS);
    if (!ExpectedID)
      return ExpectedID.takeError();
    IDs.push_back(*ExpectedID);
    KindStrings.push_back(std::make_pair(KindStrings.size(), KS));
    assert(KindStrings.size() < UCHAR_MAX &&
           "Ran out of bits for kind strings");
  }

  auto ExpectedTypeID = CAS.createNode(IDs, "cas.o:nestedv1:root");
  if (!ExpectedTypeID)
    return ExpectedTypeID.takeError();
  RootNodeTypeID = *ExpectedTypeID;
  return Error::success();
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

Expected<EncodedDataRef>
EncodedDataRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences())
    return createStringError(inconvertibleErrorCode(), "corrupt encoded data");

  return EncodedDataRef(*Specific);
}

Expected<EncodedDataRef>
EncodedDataRef::create(const ObjectFileSchema &Schema,
                       function_ref<void(SmallVectorImpl<char> &)> Encode) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  Encode(B->Data);
  return get(B->build());
}

Expected<NameRef> NameRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences())
    return createStringError(inconvertibleErrorCode(), "corrupt name");

  return NameRef(*Specific);
}

Expected<NameRef> NameRef::create(const ObjectFileSchema &Schema,
                                  StringRef Name) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  B->Data.append(Name);
  return get(B->build());
}

Expected<BlockDataRef> BlockDataRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences() > 0)
    return createStringError(inconvertibleErrorCode(),
                             "corrupt block data; got " +
                                 Twine(Specific->getNumReferences()) + " refs");

  return BlockDataRef(*Specific, data::BlockData(Specific->getData()));
}

cl::opt<size_t> MaxEdgesToEmbedInBlock(
    "max-edges-to-embed-in-block",
    cl::desc("Maximum number of edges to embed in a block."), cl::init(8));

Expected<BlockDataRef> BlockDataRef::createImpl(
    const ObjectFileSchema &Schema, Optional<StringRef> Content, uint64_t Size,
    uint64_t Alignment, uint64_t AlignmentOffset, ArrayRef<Fixup> Fixups) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  data::BlockData::encode(Size, Alignment, AlignmentOffset, Content, Fixups,
                          B->Data);
  return get(B->build());
}

// FIXME: Thread this through the API instead of sneaking it through directly
// on the command-line... or delete the option.
cl::opt<bool> ZeroFixupsDuringBlockIngestion(
    "zero-fixups-during-block-ingestion",
    cl::desc("Zero out fixups during block ingestion."), cl::init(true));

// FIXME: Thread this through the API instead of sneaking it through directly
// on the command-line... or delete the option.
cl::opt<bool> AppendTrailingNopPaddingDuringBlockIngestion__text(
    "append-trailing-nop-padding-during-block-ingestion-__text",
    cl::desc("Append trailing no-op padding to the final block in "
             "__TEXT,__text during block ingestion."),
    cl::init(true));

Expected<BlockDataRef> BlockDataRef::create(const ObjectFileSchema &Schema,
                                            const jitlink::Block &Block,
                                            ArrayRef<Fixup> Fixups) {
  if (Block.isZeroFill())
    return createZeroFill(Schema, Block.getSize(), Block.getAlignment(),
                          Block.getAlignmentOffset(), Fixups);

  assert(Block.getContent().size() == Block.getSize());
  StringRef Content(Block.getContent().begin(), Block.getSize());

  ArrayRef<Fixup> FixupsToZero;
  if (ZeroFixupsDuringBlockIngestion)
    FixupsToZero = Fixups;

  // Append trailing noops to the last block in __TEXT,__text to canonicalize
  // its shape, to avoid noise when a block happens to be last. This is because
  // we don't know the actual size of the symbol, and when it's not last it
  // already has the padding.
  //
  // FIXME: Only do this when reading blocks parsed from Mach-O.
  //
  // FIXME: Probably we want this elsewhere (not just __TEXT,__text).
  Optional<Align> TrailingNopsAlignment;
  if (AppendTrailingNopPaddingDuringBlockIngestion__text &&
      Block.getSection().getName() == "__TEXT,__text")
    TrailingNopsAlignment = Align(16);

  // Canoncalize (and update) Content.
  //
  // FIXME: Hardcoded to x86_64 right now! But that's all that
  // CompileUnitRef::create() will send over anyway.
  SmallString<1024> MutableContentStorage;
  if (Error E = helpers::canonicalizeContent(
                    Triple::x86_64, Content, MutableContentStorage,
                    FixupsToZero, TrailingNopsAlignment)
                    .moveInto(Content))
    return std::move(E);

  return createContent(Schema, Content, Block.getAlignment(),
                       Block.getAlignmentOffset(), Fixups);
}

Expected<BlockDataRef>
BlockDataRef::createZeroFill(const ObjectFileSchema &Schema, uint64_t Size,
                             uint64_t Alignment, uint64_t AlignmentOffset,
                             ArrayRef<Fixup> Fixups) {
  return createImpl(Schema, None, Size, Alignment, AlignmentOffset, Fixups);
}

Expected<BlockDataRef>
BlockDataRef::createContent(const ObjectFileSchema &Schema, StringRef Content,
                            uint64_t Alignment, uint64_t AlignmentOffset,
                            ArrayRef<Fixup> Fixups) {
  return createImpl(Schema, Content, Content.size(), Alignment, AlignmentOffset,
                    Fixups);
}

Expected<Optional<NameRef>> TargetRef::getName() const {
  if (getKind() == IndirectSymbol)
    return NameRef::get(*Schema, ID);
  auto Symbol = SymbolRef::get(*Schema, ID);
  if (!Symbol)
    return Symbol.takeError();
  return Symbol->getName();
}

Expected<TargetRef> TargetRef::get(const ObjectFileSchema &Schema,
                                   cas::CASID ID, Optional<Kind> ExpectedKind) {
  auto checkExpectedKind = [&](Kind K, StringRef Name) -> Expected<TargetRef> {
    if (ExpectedKind && *ExpectedKind != K)
      return createStringError(inconvertibleErrorCode(),
                               "unexpected " + Name + " for target");
    return TargetRef(Schema, ID, K);
  };

  auto Object = ObjectFormatNodeProxy::get(Schema, Schema.CAS.getNode(ID));
  if (!Object) {
    consumeError(Object.takeError());
    return createStringError(inconvertibleErrorCode(), "invalid target");
  }
  if (Object->getKindString() == NameRef::KindString)
    return checkExpectedKind(IndirectSymbol, "indirect symbol");
  else if (Object->getKindString() == SymbolRef::KindString)
    return checkExpectedKind(Symbol, "symbol");
  return createStringError(inconvertibleErrorCode(),
                           "invalid object kind '" + Object->getKindString() +
                               "' for a target");
}

StringRef SymbolDefinitionRef::getKindString(Kind K) {
  switch (K) {
  case Alias:
    return SymbolRef::KindString;
  case IndirectAlias:
    return NameRef::KindString;
  case Block:
    return BlockRef::KindString;
  }
}

Expected<SymbolDefinitionRef>
SymbolDefinitionRef::get(Expected<ObjectFormatNodeProxy> Ref,
                         Optional<Kind> ExpectedKind) {
  if (!Ref)
    return Ref.takeError();
  Optional<Kind> K = StringSwitch<Optional<Kind>>(Ref->getKindString())
                         .Case(SymbolRef::KindString, Alias)
                         .Case(NameRef::KindString, IndirectAlias)
                         .Case(BlockRef::KindString, Block)
                         .Default(None);
  if (!K)
    return createStringError(inconvertibleErrorCode(),
                             "invalid object kind '" + Ref->getKindString() +
                                 "' for a target");

  if (ExpectedKind && *K != *ExpectedKind)
    return createStringError(inconvertibleErrorCode(),
                             "unexpected object kind '" + Ref->getKindString() +
                                 "', expected '" +
                                 getKindString(*ExpectedKind) + "'");

  return SymbolDefinitionRef(*Ref, *K);
}

Expected<TargetListRef>
TargetListRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (!Specific->getData().empty())
    return createStringError(inconvertibleErrorCode(), "corrupt target list");
  return TargetListRef(*Specific);
}

Expected<TargetListRef> TargetListRef::create(const ObjectFileSchema &Schema,
                                              ArrayRef<TargetRef> Targets) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  B->IDs.append(Targets.begin(), Targets.end());
  return get(B->build());
}

Expected<SectionRef> SectionRef::create(const ObjectFileSchema &Schema,
                                        NameRef SectionName,
                                        jitlink::MemProt MemProt) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  B->IDs.push_back(SectionName);

  // FIXME: Does 1 byte leave enough space for expansion? Probably, but
  // 4 bytes would be fine too.
  B->Data.push_back(uint8_t(data::encodeProtectionFlags(MemProt)));
  return get(B->build());
}

jitlink::MemProt SectionRef::getMemProt() const {
  return decodeProtectionFlags((data::SectionProtectionFlags)getData()[0]);
}

Expected<SectionRef> SectionRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences() != 1 || Specific->getData().size() != 1)
    return createStringError(inconvertibleErrorCode(), "corrupt section");

  return SectionRef(*Specific);
}

Optional<size_t> BlockRef::getTargetsIndex() const {
  return Flags.HasTargets ? 2 : Optional<size_t>();
}

Optional<cas::CASID> BlockRef::getTargetInfoID() const {
  assert(Flags.HasEdges && "Expected edges");
  assert(!Flags.HasEmbeddedTargetInfo && "Expected explicit edges");
  return getReferenceID(2U + unsigned(Flags.HasTargets));
}

Expected<FixupList> BlockRef::getFixups() const {
  if (!hasEdges())
    return FixupList("");

  if (Expected<BlockDataRef> Data = getBlockData())
    return Data->getFixups();
  else
    return Data.takeError();
}

Expected<TargetInfoList> BlockRef::getTargetInfo() const {
  if (!hasEdges())
    return TargetInfoList("");

  if (Flags.HasEmbeddedTargetInfo)
    return TargetInfoList(getData().drop_front());

  Optional<cas::CASID> TargetInfoID = getTargetInfoID();
  if (!TargetInfoID)
    return TargetInfoList("");

  if (Expected<EncodedDataRef> TIs =
          EncodedDataRef::get(getSchema(), *TargetInfoID))
    return TargetInfoList(TIs->getEncodedList());
  else
    return TIs.takeError();
}

Expected<TargetList> BlockRef::getTargets() const {
  Optional<size_t> TargetsIndex = getTargetsIndex();
  if (!TargetsIndex)
    return TargetList();
  if (Flags.HasTargetInline)
    return TargetList(*this, *TargetsIndex, *TargetsIndex + 1);
  if (Expected<TargetListRef> Targets =
          TargetListRef::get(getSchema(), getReferenceID(*TargetsIndex)))
    return Targets->getTargets();
  else
    return Targets.takeError();
}

Expected<SectionRef> SectionRef::create(const ObjectFileSchema &Schema,
                                        const jitlink::Section &S) {
  Expected<NameRef> Name = NameRef::create(Schema, S.getName());
  if (!Name)
    return Name.takeError();
  return create(Schema, *Name, S.getMemProt());
}

static void
visitEdgesInStableOrder(const jitlink::Block &Block,
                        function_ref<void(const jitlink::Edge &E)> HandleEdge) {
  SmallVector<const jitlink::Edge *, 16> Edges;
  Edges.reserve(Block.edges_size());
  for (const jitlink::Edge &E : Block.edges()) {
    Edges.push_back(&E);
  }

  std::stable_sort(
      Edges.begin(), Edges.end(),
      [](const jitlink::Edge *LHS, const jitlink::Edge *RHS) {
        // Compare the fixup.
        if (LHS->getOffset() != RHS->getOffset())
          return LHS->getOffset() < RHS->getOffset();
        if (LHS->getKind() != RHS->getKind())
          return LHS->getKind() < RHS->getKind();

        // Compare the target and addend. In practice this is
        // likely dead code.
        return helpers::compareSymbolsBySemanticsAnd(
            &LHS->getTarget(), &RHS->getTarget(),
            [&LHS, &RHS](const jitlink::Symbol *, const jitlink::Symbol *) {
              return LHS->getAddend() < RHS->getAddend();
            });
      });

  for (auto *E : Edges)
    HandleEdge(*E);
}

static Error decomposeAndSortEdges(
    const jitlink::Block &Block, SmallVectorImpl<Fixup> &Fixups,
    SmallVectorImpl<TargetInfo> &TIs, SmallVectorImpl<TargetRef> &Targets,
    function_ref<Expected<Optional<TargetRef>>(const jitlink::Symbol &)>
        GetTargetRef) {
  assert(Fixups.empty());
  assert(TIs.empty());
  assert(Targets.empty());
  if (!Block.edges_size())
    return Error::success();

  struct EdgeTarget {
    const jitlink::Symbol *Symbol;
    // Index in the edge array for the edge that this target is part of.
    size_t EdgeIdx;
  };
  SmallVector<EdgeTarget, 16> EdgeTargets;
  Fixups.reserve(Block.edges_size());
  TIs.reserve(Block.edges_size());

  constexpr size_t AbstractBackedgeIndexPlaceholder = ~0ULL;

  visitEdgesInStableOrder(Block, [&](const jitlink::Edge &E) {
    Fixups.push_back(Fixup{E.getKind(), E.getOffset()});
    // We'll fill the real index later, using \p
    // AbstractBackedgeIndexPlaceholder to mark as 'unset'.
    TIs.push_back(TargetInfo{E.getAddend(), AbstractBackedgeIndexPlaceholder});
    EdgeTargets.push_back(EdgeTarget{&E.getTarget(), Fixups.size() - 1});
  });

  // Make the order of targets stable.
  std::stable_sort(EdgeTargets.begin(), EdgeTargets.end(),
                   [&Fixups](const EdgeTarget &LHS, const EdgeTarget &RHS) {
                     if (LHS.Symbol != RHS.Symbol)
                       return helpers::compareSymbolsBySemanticsAnd(
                           LHS.Symbol, RHS.Symbol,
                           helpers::compareSymbolsByAddress);

                     // Same symbol but different target means that either it's
                     // a different kind of reference, or a block got broken up.
                     auto LKind = Fixups[LHS.EdgeIdx].Kind;
                     auto RKind = Fixups[RHS.EdgeIdx].Kind;
                     if (LKind != RKind)
                       return LKind < RKind;

                     // Give up.
                     return false;
                   });

  // Collect targets, filtering out duplicate symbols.

  // Pair of CASID for \p TargetRef and its index in the \p Targets array.
  SmallDenseMap<cas::CASID, size_t, 16> SeenSymbolRefs;
  for (const EdgeTarget &EdgeTarget : EdgeTargets) {
    Expected<Optional<TargetRef>> TargetOrAbstractBackedge =
        GetTargetRef(*EdgeTarget.Symbol);
    if (!TargetOrAbstractBackedge)
      return TargetOrAbstractBackedge.takeError();

    if (!*TargetOrAbstractBackedge) {
      continue;
    }

    // Unique the targets themselves.
    TargetRef Target = **TargetOrAbstractBackedge;
    decltype(SeenSymbolRefs)::iterator SeenSymIt;
    bool Inserted;
    std::tie(SeenSymIt, Inserted) =
        SeenSymbolRefs.insert(std::make_pair(Target.getID(), Targets.size()));
    if (Inserted) {
      Targets.push_back(Target);
    }
    // Fill in the index map.
    assert(EdgeTarget.EdgeIdx < TIs.size());
    assert(SeenSymIt->second < Targets.size());
    TIs[EdgeTarget.EdgeIdx].Index = SeenSymIt->second;
  }

  // Missing indices are because of abstract backedges, set them to the index
  // past the \p Targets array.
  for (TargetInfo &TI : TIs) {
    if (TI.Index == AbstractBackedgeIndexPlaceholder)
      TI.Index = Targets.size();
  }

  return Error::success();
}

Expected<BlockRef> BlockRef::create(
    const ObjectFileSchema &Schema, const jitlink::Block &Block,
    function_ref<Expected<Optional<TargetRef>>(const jitlink::Symbol &)>
        GetTargetRef) {
  Expected<SectionRef> Section = SectionRef::create(Schema, Block.getSection());
  if (!Section)
    return Section.takeError();

  // Break down the edges.
  SmallVector<Fixup, 16> Fixups;
  SmallVector<TargetInfo, 16> TIs;
  SmallVector<TargetRef, 16> Targets;
  if (Error E =
          decomposeAndSortEdges(Block, Fixups, TIs, Targets, GetTargetRef))
    return std::move(E);
  assert(Fixups.size() == TIs.size());
  assert(Targets.size() <= Fixups.size());
  assert(Targets.empty() == Fixups.empty());

  // Return early if this is a leaf block.
  Expected<BlockDataRef> Data = BlockDataRef::create(Schema, Block, Fixups);
  if (!Data)
    return Data.takeError();
  return createImpl(Schema, *Section, *Data, TIs, Targets, Fixups);
}

cl::opt<bool>
    InlineUnaryTargetLists("inline-unary-target-lists",
                           cl::desc("Whether to inline unary target lists."),
                           cl::init(true));

Expected<BlockRef> BlockRef::createImpl(const ObjectFileSchema &Schema,
                                        SectionRef Section, BlockDataRef Data,
                                        ArrayRef<TargetInfo> TargetInfo,
                                        ArrayRef<TargetRef> Targets,
                                        ArrayRef<Fixup> Fixups) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  B->IDs.append({Section, Data});

  bool HasAbstractBackedge = false;
  for (const auto &TI : TargetInfo) {
    assert(TI.Index <= Targets.size() && "Target index out of range");
    HasAbstractBackedge |= TI.Index == Targets.size();
  }

  bool HasKeepAliveEdge = false;
  for (const auto &F : Fixups) {
    HasKeepAliveEdge |= (F.Kind == jitlink::Edge::KeepAlive);
  }

  if (TargetInfo.empty()) {
    assert(Targets.empty() && "Targets without fixups?");
  } else {
    assert((HasAbstractBackedge || !Targets.empty()) &&
           "Fixups without targets?");
  }

  const bool HasEmbeddedTargetInfo =
      !TargetInfo.empty() && TargetInfo.size() <= MaxEdgesToEmbedInBlock;
  const bool HasTargetInline = InlineUnaryTargetLists && Targets.size() == 1;
  if (HasTargetInline) {
    B->IDs.push_back(Targets[0].getID());
  } else if (!Targets.empty()) {
    auto TargetsRef = TargetListRef::create(Schema, Targets);
    if (!TargetsRef)
      return TargetsRef.takeError();
    B->IDs.push_back(*TargetsRef);
  }

  unsigned Bits = 0;
  Bits |= unsigned(!TargetInfo.empty());   // HasEdges
  Bits |= unsigned(!Targets.empty()) << 1; // HasTargets
  Bits |= unsigned(HasTargetInline) << 2;
  Bits |= unsigned(HasAbstractBackedge) << 3;
  Bits |= unsigned(HasEmbeddedTargetInfo) << 4;
  Bits |= unsigned(HasKeepAliveEdge) << 5;
  B->Data.push_back(static_cast<unsigned char>(Bits));

  if (HasEmbeddedTargetInfo) {
    TargetInfoList::encode(TargetInfo, B->Data);
  } else if (!TargetInfo.empty()) {
    auto TargetInfoRef = EncodedDataRef::create(
        Schema, [TargetInfo](SmallVectorImpl<char> &Data) {
          TargetInfoList::encode(TargetInfo, Data);
        });
    if (!TargetInfoRef)
      return TargetInfoRef.takeError();
    B->IDs.push_back(*TargetInfoRef);
  }

  return get(B->build());
}

Expected<BlockRef> BlockRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences() < 2 || Specific->getNumReferences() > 4 ||
      Specific->getData().size() < 1)
    return createStringError(inconvertibleErrorCode(), "corrupt block");

  BlockRef B(*Specific);
  unsigned char Bits = Specific->getData()[0];
  B.Flags.HasEdges = Bits & 1U;
  B.Flags.HasTargets = Bits & (1U << 1);
  B.Flags.HasTargetInline = Bits & (1U << 2);
  B.Flags.HasAbstractBackedge = Bits & (1U << 3);
  B.Flags.HasEmbeddedTargetInfo = Bits & (1U << 4);
  B.Flags.HasKeepAliveEdge = Bits & (1U << 5);
  return B;
}

Expected<TargetRef> SymbolRef::getAsIndirectTarget() const {
  Expected<Optional<NameRef>> Name = getName();
  if (!Name)
    return Name.takeError();
  if (!*Name)
    return createStringError(
        inconvertibleErrorCode(),
        "invalid attempt to target anonymous symbol using its name");
  return TargetRef::getIndirectSymbol(getSchema(), **Name);
}

Expected<SymbolRef> SymbolRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  // Check there are the right number of references.
  if (Specific->getNumReferences() != 1 && Specific->getNumReferences() != 2)
    return createStringError(inconvertibleErrorCode(), "corrupt symbol");

  // Check that the linkage is valid.
  if (Specific->getData().empty())
    return createStringError(inconvertibleErrorCode(),
                             "corrupt symbol linkage");

  uint64_t Offset = 0;
  if (Specific->getData().size() > 1) {
    StringRef Remaining = Specific->getData().drop_front();
    Error E = encoding::consumeVBR8(Remaining, Offset);
    if (E || !Remaining.empty()) {
      consumeError(std::move(E));
      return createStringError(inconvertibleErrorCode(),
                               "corrupt symbol offset");
    }
  }

  // Anonymous symbols cannot be exported or merged by name.
  SymbolRef Symbol(*Specific, Offset);
  if ((uint8_t)Symbol.getDeadStrip() > (uint8_t)DS_Max ||
      (uint8_t)Symbol.getScope() > (uint8_t)S_Max ||
      (uint8_t)Symbol.getMerge() > (uint8_t)M_Max)
    return createStringError(inconvertibleErrorCode(),
                             "corrupt symbol linkage");
  if ((!Symbol.hasName()) &&
      (Symbol.getScope() != S_Local || (Symbol.getMerge() & M_ByName)))
    return createStringError(inconvertibleErrorCode(),
                             "corrupt anonymous symbol");

  return Symbol;
}

bool SymbolRef::isSymbolTemplate() const {
  return (unsigned char)getData()[0] & 0x1U;
}

cl::opt<bool> UseAutoHideDuringIngestion(
    "use-autohide-during-ingestion",
    cl::desc("Use Mach-O autohide bit during ingestion."), cl::init(true));

cl::opt<bool> UseDeadStripCompileForLocals(
    "use-dead-strip-compile-for-locals",
    cl::desc("Use DeadStrip=CompileUnit for local symbols."), cl::init(true));

cl::opt<bool> DeadStripByDefault(
    "dead-strip",
    cl::desc("Dead-strip unreferenced symbols in any section. Use "
             "--dead-strip-section to just dead strip named sections."),
    cl::init(false));

cl::list<std::string> DeadStripSections(
    "dead-strip-section",
    cl::desc("Dead-strip unreferenced symbols in the named sections."));

cl::list<std::string> KeepAliveSections(
    "keep-alive-section",
    cl::desc("Keep all unreferenced symbols in the named sections. "
             "Stronger than --dead-strip or --dead-strip-section."));

cl::opt<bool>
    KeepStaticInitializersAlive("keep-static-initializers-alive",
                                cl::desc("Keep '__TEXT,__constructor' alive."),
                                cl::init(true));

// FIXME: This should be removed once compact unwind is split up and has
// KeepAlive edges.
cl::opt<bool>
    KeepCompactUnwindAlive("keep-compact-unwind-alive",
                           cl::desc("Keep '__LD,__compact_unwind' alive."),
                           cl::init(true));

static bool shouldKeepAliveInSection(StringRef SectionName) {
  static StringSet<> KeepAlive;
  static once_flag Once;
  llvm::call_once(Once, [&]() {
    for (StringRef S : KeepAliveSections)
      KeepAlive.insert(S);

    // FIXME: Stop using hardcoded section names.
    if (KeepStaticInitializersAlive) {
      KeepAlive.insert("__DATA,__mod_init_func");
      KeepAlive.insert("__TEXT,__constructor");
    }
    if (KeepCompactUnwindAlive)
      KeepAlive.insert("__LD,__compact_unwind");
  });
  return KeepAlive.count(SectionName);
}

static bool shouldDeadStripInSection(StringRef SectionName) {
  static StringSet<> DeadStrip;
  static once_flag Once;
  llvm::call_once(Once, [&]() {
    for (StringRef S : DeadStripSections)
      DeadStrip.insert(S);
  });
  return DeadStrip.count(SectionName);
}

static bool shouldKeepAlive(const jitlink::Symbol &S) {
  return S.isDefined() &&
         shouldKeepAliveInSection(S.getBlock().getSection().getName());
}

static bool shouldDeadStrip(const jitlink::Symbol &S) {
  if (DeadStripByDefault)
    return true;
  return !S.isDefined() ||
         shouldDeadStripInSection(S.getBlock().getSection().getName());
}

SymbolRef::Flags SymbolRef::getFlags(const jitlink::Symbol &S) {
  Flags F;
  switch (S.getScope()) {
  case jitlink::Scope::Default:
    F.Scope = S_Global;
    break;
  case jitlink::Scope::Hidden:
    F.Scope = S_Hidden;
    break;
  case jitlink::Scope::Local:
    F.Scope = S_Local;
    break;
  };

  bool IsAutoHide = UseAutoHideDuringIngestion && S.isAutoHide();

  // FIXME: Can we detect M_ByContent in more cases?
  F.Merge = SymbolRef::MergeKind(
      (S.getLinkage() == jitlink::Linkage::Strong ? M_Never : M_ByName) |
      (IsAutoHide ? M_ByContent : M_Never));

  // FIXME: Can we detect DS_CompileUnit in more cases?
  F.DeadStrip = (S.isLive() || shouldKeepAlive(S))
                    ? DS_Never
                    : (IsAutoHide || (UseDeadStripCompileForLocals &&
                                      S.getScope() == jitlink::Scope::Local))
                          ? DS_CompileUnit
                          : DS_LinkUnit;

  return F;
}

Expected<SymbolRef> SymbolRef::create(
    const ObjectFileSchema &Schema, const jitlink::Symbol &S,
    function_ref<Expected<SymbolDefinitionRef>(const jitlink::Block &)>
        GetDefinitionRef) {
  assert(!S.isExternal() && "Expected defined symbol");
  assert(!S.isAbsolute() && "Absolute symbols not yet implemented");
  Expected<SymbolDefinitionRef> Definition = GetDefinitionRef(S.getBlock());
  if (!Definition)
    return Definition.takeError();

  Optional<NameRef> Name;
  if (!S.getName().empty()) {
    Expected<NameRef> ExpectedName = NameRef::create(Schema, S.getName());
    if (!ExpectedName)
      return ExpectedName.takeError();

    Name = *ExpectedName;
  }

  return create(Schema, Name, *Definition, S.getOffset(), getFlags(S));
}

Expected<SymbolRef> SymbolRef::create(const ObjectFileSchema &Schema,
                                      Optional<NameRef> SymbolName,
                                      SymbolDefinitionRef Definition,
                                      uint64_t Offset, Flags F) {
  // Anonymous symbols cannot be exported or merged by name.
  if (!SymbolName && (F.Scope != S_Local || (F.Merge & M_ByName)))
    return createStringError(inconvertibleErrorCode(),
                             "invalid anonymous symbol");

  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  bool IsSymbolTemplate = false;
  if (Definition.getKind() == SymbolDefinitionRef::Block)
    IsSymbolTemplate =
        cantFail(BlockRef::get(Definition)).hasAbstractBackedge();

  static_assert(((uint8_t)DS_Max >> 2) == 0, "Not enough bits for dead-strip");
  static_assert(((uint8_t)S_Max >> 2) == 0, "Not enough bits for scope");
  static_assert(((uint8_t)M_Max >> 2) == 0, "Not enough bits for merge");
  B->Data.push_back((uint8_t)F.DeadStrip << 6 | (uint8_t)F.Scope << 4 |
                    (uint8_t)F.Merge << 2 | (uint8_t)IsSymbolTemplate);
  if (Offset)
    encoding::writeVBR8(Offset, B->Data);

  B->IDs.push_back(Definition);
  if (SymbolName)
    B->IDs.push_back(*SymbolName);

  return get(B->build());
}

Expected<CompileUnitRef>
CompileUnitRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences() != 8)
    return createStringError(inconvertibleErrorCode(),
                             "corrupt compile unit refs");

  // Parse the fields stored in the data.
  StringRef Remaining = Specific->getData();
  uint32_t PointerSize;
  uint32_t NormalizedTripleSize;
  uint8_t Endianness;
  Error E = encoding::consumeVBR8(Remaining, PointerSize);
  if (!E)
    E = encoding::consumeVBR8(Remaining, NormalizedTripleSize);
  if (!E)
    E = encoding::consumeVBR8(Remaining, Endianness);
  if (E) {
    consumeError(std::move(E));
    return createStringError(inconvertibleErrorCode(),
                             "corrupt compile unit data");
  }
  if (Endianness != uint8_t(support::endianness::little) &&
      Endianness != uint8_t(support::endianness::big))
    return createStringError(inconvertibleErrorCode(),
                             "corrupt compile unit endianness");
  if (Remaining.size() != NormalizedTripleSize)
    return createStringError(inconvertibleErrorCode(),
                             "corrupt compile unit triple");

  return CompileUnitRef(*Specific, Triple(Remaining), PointerSize,
                        support::endianness(Endianness));
}

Expected<CompileUnitRef> CompileUnitRef::create(
    const ObjectFileSchema &Schema, const Triple &TT, unsigned PointerSize,
    support::endianness Endianness, SymbolTableRef DeadStripNever,
    SymbolTableRef DeadStripLink, SymbolTableRef IndirectDeadStripCompile,
    SymbolTableRef IndirectAnonymous, NameListRef StrongSymbols,
    NameListRef WeakSymbols, SymbolTableRef Unreferenced) {
  Expected<Builder> B = Builder::startRootNode(Schema, KindString);
  if (!B)
    return B.takeError();

  std::string NormalizedTriple = TT.normalize();
  encoding::writeVBR8(uint32_t(PointerSize), B->Data);
  encoding::writeVBR8(uint32_t(NormalizedTriple.size()), B->Data);
  encoding::writeVBR8(uint8_t(Endianness), B->Data);
  B->Data.append(NormalizedTriple);

  assert(B->IDs.size() == 1 && "Expected the root type-id?");
  B->IDs.append({DeadStripNever, DeadStripLink, IndirectDeadStripCompile,
                 IndirectAnonymous, StrongSymbols, WeakSymbols, Unreferenced});
  return get(B->build());
}

Expected<NameListRef> NameListRef::create(const ObjectFileSchema &Schema,
                                          MutableArrayRef<NameRef> Names) {
  if (Names.size() >= (1ull << 32))
    return createStringError(inconvertibleErrorCode(), "too many names");

  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  // Sort the names to create a stable order.
  if (Names.size() > 1) {
    llvm::sort(Names, [](const NameRef &LHS, const NameRef &RHS) {
      return LHS.getName() < RHS.getName();
    });
  }
  B->IDs.append(Names.begin(), Names.end());

  return get(B->build());
}

Expected<NameListRef> NameListRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (!Specific->getData().empty())
    return createStringError(inconvertibleErrorCode(), "corrupt name list");

  return NameListRef(*Specific);
}

Expected<SymbolTableRef> SymbolTableRef::create(const ObjectFileSchema &Schema,
                                                ArrayRef<SymbolRef> Symbols) {
  if (Symbols.size() >= (1ull << 32))
    return createStringError(inconvertibleErrorCode(), "too many symbols");

  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  // Sort the symbols to create a stable order.
  uint32_t NumAnonymousSymbols = 0;
  if (Symbols.size() == 1) {
    B->IDs.push_back(Symbols[0]);
    if (!Symbols[0].hasName())
      ++NumAnonymousSymbols;
  } else if (Symbols.size() > 1) {
    SmallVector<StringRef, 32> Names;
    SmallVector<uint32_t, 32> Order;
    Names.reserve(Symbols.size());
    Order.reserve(Symbols.size());

    // Initialize the symbol order and cache the symbol names.
    for (SymbolRef S : Symbols) {
      Order.push_back(Order.size());
      if (!S.hasName()) {
        ++NumAnonymousSymbols;
        Names.push_back("");
        continue;
      }
      Expected<Optional<NameRef>> Name = S.getName();
      if (!Name)
        return Name.takeError();
      Names.push_back((*Name)->getName());
    }

    // FIXME: Consider sorting by target as well (e.g., block size) to make
    // order more stable.
    std::stable_sort(Order.begin(), Order.end(),
                     [&Names, &Symbols](uint32_t LHS, uint32_t RHS) {
                       if (int Diff = Names[LHS].compare(Names[RHS]))
                         return Diff < 0;
                       return Symbols[LHS].getFlags() < Symbols[RHS].getFlags();
                     });

    // Spot check that anonymous symbols are first.
    if (NumAnonymousSymbols) {
      assert(!Symbols[Order[0]].hasName());
      assert(!Symbols[Order[NumAnonymousSymbols - 1]].hasName());
    }
    if (NumAnonymousSymbols != Symbols.size()) {
      assert(Symbols[Order[NumAnonymousSymbols]].hasName());
      assert(Symbols[Order[Order.size() - 1]].hasName());
    }

    // Remove duplicates.
    //
    // FIXME: Is this the right thing to do? It avoids complexity when reading,
    // but maybe we need to be able to define symbols that are exact
    // duplicates for some reason. E.g., this prevents adding support for
    // referencing anonymous symbols indirectly in a way that doesn't modify
    // the symbol itself. Maybe that's okay?
    DenseSet<cas::CASID> UniqueIDs;
    for (uint32_t I : Order)
      if (UniqueIDs.insert(Symbols[I]).second)
        B->IDs.push_back(Symbols[I]);
      else if (Names[I].empty())
        --NumAnonymousSymbols;
  }

  // Write the number of anonymous symbols.
  encoding::writeVBR8(uint32_t(NumAnonymousSymbols), B->Data);

  // FIXME: Also write out an on-disk hash table in Data for fast lookup, if
  // there are more than a few.
  //
  // DO NOT use a prefix of Name.getHash() as the hash, since will change
  // depending on the exact CAS being used.
  //
  // We can be space efficient in the symbol table and avoid storing the key
  // (the names) at all, just the value (offset into references). On a hit,
  // need to check that the symbol does indeed have the right name, which isn't
  // free... there's a trade-off.
  return get(B->build());
}

Expected<Optional<SymbolRef>> SymbolTableRef::lookupSymbol(NameRef Name) const {
  // Do a binary search.
  uint32_t F = getNumAnonymousSymbols();
  uint32_t L = getNumSymbols();
  while (F != L) {
    uint32_t I = (F + L) / 2;
    Expected<SymbolRef> Symbol = getSymbol(I);
    if (!Symbol)
      return Symbol.takeError();

    // Check for an exact match.
    assert(Symbol->hasName() && "Expected named symbols...");
    if (*Symbol->getNameID() == Name)
      return *Symbol;

    Expected<Optional<NameRef>> IName = Symbol->getName();
    if (!IName)
      return IName.takeError();

    assert(**IName != Name.getID() && "Already checked for equality");
    if (Name.getName() < (**IName).getName())
      L = I;
    else
      F = I + 1;
  }
  return None;
}

Expected<SymbolTableRef>
SymbolTableRef::get(Expected<ObjectFormatNodeProxy> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  StringRef Remaining = Specific->getData();
  uint32_t NumAnonymousSymbols;
  Error E = encoding::consumeVBR8(Remaining, NumAnonymousSymbols);
  if (E || !Remaining.empty()) {
    consumeError(std::move(E));
    return createStringError(inconvertibleErrorCode(), "corrupt symbol table");
  }

  return SymbolTableRef(*Specific, NumAnonymousSymbols);
}

namespace {
struct SymbolGraph {
  const jitlink::Symbol &S;
  SymbolGraph(const jitlink::Symbol &S) : S(S) {}
};
} // namespace

namespace llvm {
template <> struct GraphTraits<SymbolGraph> {
  using NodeRef = const jitlink::Symbol *;

  static NodeRef getEntryNode(const SymbolGraph &G) { return &G.S; }

  static const jitlink::Symbol *getEdgeTarget(const jitlink::Edge &E) {
    return &E.getTarget();
  }

  using ChildIteratorType = mapped_iterator<
      jitlink::Block::const_edge_iterator,
      const llvm::jitlink::Symbol *(*)(const llvm::jitlink::Edge &)>;

  static ChildIteratorType child_begin(NodeRef N) {
    if (!N->isDefined())
      return ChildIteratorType(jitlink::Block::const_edge_iterator(),
                               getEdgeTarget);
    const jitlink::Block &B = N->getBlock();
    return map_iterator(B.edges().begin(), getEdgeTarget);
  }

  static ChildIteratorType child_end(NodeRef N) {
    if (!N->isDefined())
      return ChildIteratorType(jitlink::Block::const_edge_iterator(),
                               getEdgeTarget);
    const jitlink::Block &B = N->getBlock();
    return map_iterator(B.edges().end(), getEdgeTarget);
  }
};
} // namespace llvm

namespace {
class CompileUnitBuilder {
public:
  cas::CASDB &CAS;
  const ObjectFileSchema &Schema;
  raw_ostream *DebugOS;

  CompileUnitBuilder(const ObjectFileSchema &Schema, raw_ostream *DebugOS)
      : CAS(Schema.CAS), Schema(Schema), DebugOS(DebugOS) {}

  struct SymbolInfo {
    Optional<NameRef> Indirect;
    Optional<SymbolRef> Symbol;
  };
  DenseMap<const jitlink::Symbol *, SymbolInfo> Symbols;
  DenseMap<const jitlink::Block *, BlockRef> Blocks;

  SmallVector<SymbolRef> DeadStripNeverSymbols;
  SmallVector<SymbolRef> DeadStripLinkSymbols;
  SmallVector<SymbolRef> IndirectDeadStripCompileSymbols;
  SmallVector<SymbolRef> IndirectAnonymousSymbols;
  SmallVector<SymbolRef> UnreferencedSymbols;
  MapVector<const jitlink::Symbol *, bool> UnreferencedSymbolsMap;
  SmallVector<NameRef> StrongExternals;
  SmallVector<NameRef> WeakExternals;

  /// Make symbols in post-order, exported symbols and non-dead-strippable
  /// symbols as entry points.
  Error makeSymbols(const jitlink::LinkGraph &G);

  /// Create the given symbol and cache it.
  Error createSymbol(const jitlink::Symbol &S);

  /// Cache the symbol. Asserts that the result is not already cached.
  void cacheSymbol(const jitlink::Symbol &S, SymbolRef Ref);

  /// Get the symbol as a target for the block. If \p S has a definition in \a
  /// Symbols, use that if it's not prefered to reference it indirectly.
  ///
  /// FIXME: Maybe targets should usually be name-based to improve redundancy
  /// between builds. In particular, could use direct references *only* for
  /// Merge=M_ByContent && Scope=S_Local. In those cases, we'd want to
  /// reference an anonymous symbol directly, and only keep the name around as
  /// an alias as a convenience for users.
  Expected<Optional<TargetRef>>
  getOrCreateTarget(const jitlink::Symbol &S,
                    const jitlink::Block &SourceBlock);

  /// Get or create a block.
  Expected<BlockRef> getOrCreateBlock(const jitlink::Block &B);

  /// Get or create a symbol definition.
  Expected<SymbolDefinitionRef>
  getOrCreateSymbolDefinition(const jitlink::Symbol &S);

private:
  /// Guaranteed to be called in post-order. All undefined targets must be
  /// name-based.
  Error makeSymbolTargetImpl(const jitlink::Symbol &S);
};
} // namespace

Error CompileUnitBuilder::makeSymbols(const jitlink::LinkGraph &G) {
  SmallPtrSet<const jitlink::Symbol *, 8> Visited;

  // Collect all the symbols, one section at a time with absolute symbols
  // (arbitrarily) last. Sort the symbols within each section by address to
  // create a stable order, since jitlink::Section stores them in a DenseSet.
  SmallVector<const jitlink::Symbol *, 16> Symbols;
  auto appendSymbols = [&](auto &&NewSymbols) {
    size_t PreviousSize = Symbols.size();
    Symbols.append(NewSymbols.begin(), NewSymbols.end());
    llvm::sort(Symbols.begin() + PreviousSize, Symbols.end(),
               helpers::compareSymbolsByAddress);
  };
  for (const jitlink::Section &Section : G.sections())
    appendSymbols(Section.symbols());
  appendSymbols(G.absolute_symbols());

  // Sort all symbols in an address-independent way to prepare for visiting
  // them. Use a stable sort so that the previous sort-by-section-then-address
  // will break ties.
  std::stable_sort(Symbols.begin(), Symbols.end(),
                   helpers::compareSymbolsByLinkageAndSemantics);

  // Make exported symbols with strong linkage. Delay weak and internal symbols
  // to make the order predictable for SCCs involving both.
  //
  // Make exported symbols with weak linkage, then locals.
  //
  // FIXME: Consider skipping symbols that aren't used.
  for (const jitlink::Symbol *Entry : Symbols) {
    if (Visited.count(Entry))
      continue;

    // FIXME: Consider skipping this symbol if the format would implicitly
    // dead-strip it anyway.

    // Use a simple post-order traversal, rather than treating members of an
    // SCC as peers, in order to build dominators of SCCs last.
    for (const jitlink::Symbol *S :
         post_order_ext(SymbolGraph(*Entry), Visited))
      if (!S->isExternal())
        if (Error E = createSymbol(*S))
          return E;
  }

  // Fill the UnreferencedSymbols table.
  for (const auto &I : UnreferencedSymbolsMap)
    if (!I.second)
      UnreferencedSymbols.push_back(*this->Symbols.lookup(I.first).Symbol);

  return Error::success();
}

cl::opt<bool> DropLeafSymbolNamesWithDot(
    "drop-leaf-symbol-names-with-dot",
    cl::desc("Drop symbol names with '.' in them from leafs."), cl::init(true));

Error CompileUnitBuilder::createSymbol(const jitlink::Symbol &S) {
  assert(!S.isExternal());
  assert(!Symbols.lookup(&S).Symbol);

  // FIXME: Handle absolute symbols.
  assert(!S.isAbsolute());

  Optional<NameRef> Name = Symbols.lookup(&S).Indirect;
  bool HasIndirectReference = bool(Name);

  Expected<SymbolDefinitionRef> Definition = getOrCreateSymbolDefinition(S);
  if (!Definition)
    return Definition.takeError();

  if (!Name && S.hasName() &&
      (!DropLeafSymbolNamesWithDot || S.getScope() != jitlink::Scope::Local ||
       S.getName().find('.') == StringRef::npos ||
       cantFail(BlockRef::get(*Definition)).hasEdges())) {
    Expected<NameRef> ExpectedName = NameRef::create(Schema, S.getName());
    if (!ExpectedName)
      return ExpectedName.takeError();
    Name = *ExpectedName;
  }

  if (DebugOS) {
    if (Name)
      *DebugOS << "symbol = " << S.getName() << "\n";
    else
      *DebugOS << "anonymous symbol\n";
  }

  SymbolRef::Flags F = SymbolRef::getFlags(S);

  Expected<SymbolRef> Symbol =
      SymbolRef::create(Schema, Name, *Definition, S.getOffset(), F);
  if (!Symbol)
    return Symbol.takeError();

  // Check if the symbol should be indexed at the top level.
  switch (Symbol->getDeadStrip()) {
  case SymbolRef::DS_Never:
    // All KeepAlive symbols end up here. That'll include attribute((used)).
    DeadStripNeverSymbols.push_back(*Symbol);
    break;
  case SymbolRef::DS_LinkUnit:
    // All symbols that the compiler was not allowed to dead-strip end up here.
    // For C++, that typically excludes inlines, templates, and local symbols,
    // which end up in the next case.
    DeadStripLinkSymbols.push_back(*Symbol);
    break;
  case SymbolRef::DS_CompileUnit:
    // Collect dead-strippable symbols that are referenced indirectly (i.e., by
    // name) in IndirectDeadStripCompileSymbols.
    //
    // Remaining symbols are added to UnreferencedSymbolsMap. If a reference is
    // added later, the value will be set to true.
    //
    // FIXME: Fine-tune what goes here. The "named" table needs anything that
    // might be found by symbol name, so that's anything that gets referenced
    // indirectly (not by a direct CAS link).
    //
    // For now that's only members of an SCC, but that's probably not the right
    // final answer. We may want to decide at this point to add other things.
    //
    // - If the address is relevant, we need to know that it belongs to a
    //   specific object file, not just any, and that things pointing at it
    //   point to the same one.
    //
    // - If there are any IndirectSymbol (by-name) references to it, it needs
    //   to be find-able by name.
    if (HasIndirectReference)
      IndirectDeadStripCompileSymbols.push_back(*Symbol);
    else if (!shouldDeadStrip(S))
      UnreferencedSymbolsMap.insert({&S, false});
    break;
  }

  cacheSymbol(S, *Symbol);
  return Error::success();
}

void CompileUnitBuilder::cacheSymbol(const jitlink::Symbol &S, SymbolRef Ref) {
  auto &Info = Symbols[&S];
  assert(!Info.Symbol && "Expected no symbol definition yet...");
  Info.Symbol = Ref;
}

Expected<SymbolDefinitionRef>
CompileUnitBuilder::getOrCreateSymbolDefinition(const jitlink::Symbol &S) {
  // If the assertion in S.getBlock() starts failing, probably LinkGraph added
  // support for aliases to external symbols.
  return SymbolDefinitionRef::get(getOrCreateBlock(S.getBlock()));
}

Expected<BlockRef>
CompileUnitBuilder::getOrCreateBlock(const jitlink::Block &B) {
  auto Cached = Blocks.find(&B);
  if (Cached != Blocks.end())
    return Cached->second;

  Expected<BlockRef> ExpectedBlock =
      BlockRef::create(Schema, B, [&](const jitlink::Symbol &S) {
        return getOrCreateTarget(S, B);
      });
  if (!ExpectedBlock)
    return ExpectedBlock.takeError();
  Cached = Blocks.insert(std::make_pair(&B, *ExpectedBlock)).first;
  return Cached->second;
}

static bool isPCBeginFromFDE(const jitlink::Symbol &S,
                             const jitlink::Block &SourceBlock) {
  // FIXME: Mach-O specific logic.
  StringLiteral EHFrameSection = "__TEXT,__eh_frame";
  if (SourceBlock.getSection().getName() != EHFrameSection)
    return false;

  // FIXME: How does this happen? Was an assertion but it failed.  Seems like
  // __TEXT,__eh_frame should only point at defined symbols.
  if (!S.isDefined())
    return false;

  // If the edge points inside the same section, it's the CIE. Otherwise, it
  // should be PCBegin.
  return S.getBlock().getSection().getName() != EHFrameSection;
}

cl::opt<bool> UseAbstractBackedgeForPCBeginInFDEDuringBlockIngestion(
    "use-abstract-backedge-for-pc-begin-in-fde-during-block-ingestion",
    cl::desc("Use an abstract backedge for PCBegin in FDEs."), cl::init(true));

cl::opt<bool>
    PreferIndirectSymbolRefs("prefer-indirect-symbol-refs",
                             cl::desc("Prefer referencing symbols indirectly."),
                             cl::init(false));

Expected<Optional<TargetRef>>
CompileUnitBuilder::getOrCreateTarget(const jitlink::Symbol &S,
                                      const jitlink::Block &SourceBlock) {
  // Use an abstract target for the back-edge from FDEs in __TEXT,__eh_frame.
  if (UseAbstractBackedgeForPCBeginInFDEDuringBlockIngestion &&
      isPCBeginFromFDE(S, SourceBlock))
    return None;

  // Mark this symbol as NOT being unreferenced.
  {
    auto I = UnreferencedSymbolsMap.find(&S);
    if (I != UnreferencedSymbolsMap.end())
      I->second = true;
  }

  // Check if the target exists already.
  auto &Info = Symbols[&S];
  if (Info.Symbol && (!PreferIndirectSymbolRefs || !Info.Symbol->hasName()))
    return Info.Symbol->getAsTarget();
  if (Info.Indirect)
    return TargetRef::getIndirectSymbol(Schema, *Info.Indirect);

  // Create an indirect target.
  auto createIndirectTarget = [&](NameRef Name) {
    Info.Indirect = Name;
    return TargetRef::getIndirectSymbol(Schema, Name);
  };

  if (Info.Symbol) {
    assert(!S.isExternal() && "External symbol has unexpected definition");
    assert(S.hasName() &&
           "Too late to create indirect reference to anonymous symbol...");

    // FIXME: Add a test confirming that this is done if and only if the
    // deadstripping is DS_CompileUnit.
    if (Info.Symbol->getDeadStrip() == SymbolRef::DS_CompileUnit)
      IndirectDeadStripCompileSymbols.push_back(*Info.Symbol);

    Expected<Optional<NameRef>> Name = Info.Symbol->getName();
    if (!Name)
      return Name.takeError();
    assert(*Name && "Symbol definition missing a name");
    assert((**Name).getName() == S.getName() && "Name mismatch");
    return createIndirectTarget(**Name);
  }

  Optional<std::string> NameStorage;
  auto getName = [&]() -> StringRef {
    if (S.hasName())
      return S.getName();

    assert(!S.isExternal());

    NameStorage = ("cas.o:" + Twine(IndirectAnonymousSymbols.size())).str();
    IndirectAnonymousSymbols.push_back(*Info.Symbol);
    if (DebugOS)
      *DebugOS << "name anonymous symbol => " << *NameStorage << "\n";
    return *NameStorage;
  };
  Expected<NameRef> Name = NameRef::create(Schema, getName());
  if (!Name)
    return Name.takeError();

  if (S.isExternal()) {
    if (S.getLinkage() == jitlink::Linkage::Weak)
      WeakExternals.push_back(*Name);
    else
      StrongExternals.push_back(*Name);
  }
  return createIndirectTarget(*Name);
}

Expected<CompileUnitRef> CompileUnitRef::create(const ObjectFileSchema &Schema,
                                                const jitlink::LinkGraph &G,
                                                raw_ostream *DebugOS) {
  if (Error E = helpers::checkArch(G.getTargetTriple()))
    return std::move(E);

  CompileUnitBuilder Builder(Schema, DebugOS);
  if (Error E = Builder.makeSymbols(G))
    return std::move(E);

  Expected<SymbolTableRef> NeverTable =
      SymbolTableRef::create(Schema, Builder.DeadStripNeverSymbols);
  if (!NeverTable)
    return NeverTable.takeError();
  Expected<SymbolTableRef> LinkTable =
      SymbolTableRef::create(Schema, Builder.DeadStripLinkSymbols);
  if (!LinkTable)
    return LinkTable.takeError();
  Expected<SymbolTableRef> CompileTable =
      SymbolTableRef::create(Schema, Builder.IndirectDeadStripCompileSymbols);
  if (!CompileTable)
    return CompileTable.takeError();
  Expected<SymbolTableRef> AnonymousTable =
      SymbolTableRef::create(Schema, Builder.IndirectAnonymousSymbols);
  if (!AnonymousTable)
    return AnonymousTable.takeError();
  Expected<NameListRef> StrongExternals =
      NameListRef::create(Schema, Builder.StrongExternals);
  if (!StrongExternals)
    return StrongExternals.takeError();
  Expected<NameListRef> WeakExternals =
      NameListRef::create(Schema, Builder.WeakExternals);
  if (!WeakExternals)
    return WeakExternals.takeError();
  Expected<SymbolTableRef> Unreferenced =
      SymbolTableRef::create(Schema, Builder.UnreferencedSymbols);
  if (!Unreferenced)
    return Unreferenced.takeError();

  return CompileUnitRef::create(Schema, G.getTargetTriple(), G.getPointerSize(),
                                G.getEndianness(), *NeverTable, *LinkTable,
                                *CompileTable, *AnonymousTable,
                                *StrongExternals, *WeakExternals, *Unreferenced);
}

namespace llvm {
template <class RefT>
struct DenseMapInfo<NestedV1ObjectReader::TypedID<RefT>>
    : public DenseMapInfo<cas::CASID> {
  static NestedV1ObjectReader::TypedID<RefT> getEmptyKey() {
    return NestedV1ObjectReader::TypedID<RefT>{
        DenseMapInfo<cas::CASID>::getEmptyKey()};
  }

  static NestedV1ObjectReader::TypedID<RefT> getTombstoneKey() {
    return NestedV1ObjectReader::TypedID<RefT>{
        DenseMapInfo<cas::CASID>::getTombstoneKey()};
  }
};
} // namespace llvm

Expected<std::unique_ptr<CASObjectReader>>
CompileUnitRef::createObjectReader() {
  return std::make_unique<NestedV1ObjectReader>(*this);
}

Error NestedV1ObjectReader::forEachSymbol(
    function_ref<Error(CASSymbolRef, CASSymbol)> Callback) const {
  if (Error E = forEachExternalSymbol(CURef.getStrongExternals(),
                                      jitlink::Linkage::Strong, Callback))
    return E;
  if (Error E = forEachExternalSymbol(CURef.getWeakExternals(),
                                      jitlink::Linkage::Weak, Callback))
    return E;
  if (Error E = forEachSymbol(CURef.getIndirectAnonymous(), Callback))
    return E;
  if (Error E = forEachSymbol(CURef.getIndirectDeadStripCompile(), Callback))
    return E;
  if (Error E = forEachSymbol(CURef.getDeadStripLink(), Callback))
    return E;
  if (Error E = forEachSymbol(CURef.getDeadStripNever(), Callback))
    return E;
  if (Error E = forEachSymbol(CURef.getUnreferenced(), Callback))
    return E;
  return Error::success();
}

Error NestedV1ObjectReader::forEachExternalSymbol(
    Expected<NameListRef> ExternalSymbols, jitlink::Linkage Linkage,
    function_ref<Error(CASSymbolRef, CASSymbol)> Callback) const {
  if (!ExternalSymbols)
    return ExternalSymbols.takeError();
  for (size_t I = 0, E = ExternalSymbols->getNumNames(); I != E; ++I) {
    auto CASSymRef = getOrCreateExternalCASSymbolRef(
        ExternalSymbols->getSchema(), ExternalSymbols->getName(I), Linkage);
    if (!CASSymRef)
      return CASSymRef.takeError();
    auto CASSym = materialize(*CASSymRef);
    if (!CASSym)
      return CASSym.takeError();
    if (Error Err = Callback(*CASSymRef, *CASSym))
      return Err;
  }
  return Error::success();
}

Error NestedV1ObjectReader::forEachSymbol(
    Expected<SymbolTableRef> Table,
    function_ref<Error(CASSymbolRef, CASSymbol)> Callback) const {
  if (!Table)
    return Table.takeError();
  for (size_t I = 0, E = Table->getNumSymbols(); I != E; ++I) {
    auto CASSymRef = getOrCreateCASSymbolRef(Table->getSymbol(I), None);
    if (!CASSymRef)
      return CASSymRef.takeError();
    auto CASSym = materialize(*CASSymRef);
    if (!CASSym)
      return CASSym.takeError();
    if (Error Err = Callback(*CASSymRef, *CASSym))
      return Err;
  }
  return Error::success();
}

Expected<CASSymbolRef> NestedV1ObjectReader::getOrCreateExternalCASSymbolRef(
    const ObjectFileSchema &Schema, Expected<NameRef> SymbolName,
    jitlink::Linkage Linkage) const {
  if (!SymbolName)
    return SymbolName.takeError();

  TargetRef Target = TargetRef::getIndirectSymbol(Schema, *SymbolName);
  std::lock_guard<sys::Mutex> Guard(SymbolsLock);
  auto &SymRef = Symbols[Target];
  if (!SymRef) {
    SymRef = std::make_unique<ExternalSymbolNodeRef>(*SymbolName, Linkage);
    SymbolsByName[SymbolName->getName()] = CASSymbolRef{uint64_t(SymRef.get())};
  }
  return CASSymbolRef{uint64_t(SymRef.get())};
}

Expected<CASSymbolRef> NestedV1ObjectReader::getOrCreateCASSymbolRef(
    Expected<SymbolRef> Symbol,
    Optional<CASSymbolRef> KeptAliveBySymbol) const {
  if (!Symbol)
    return Symbol.takeError();

  TargetRef Target = Symbol->getAsTarget();
  std::lock_guard<sys::Mutex> Guard(SymbolsLock);
  auto &Ref = Symbols[Target];
  if (!Ref) {
    Ref = std::make_unique<DefinedSymbolNodeRef>(std::move(*Symbol),
                                                 std::move(KeptAliveBySymbol));
    Expected<Optional<NameRef>> NameRef = Symbol->getName();
    if (!NameRef)
      return NameRef.takeError();
    if (NameRef->hasValue())
      SymbolsByName[(*NameRef)->getName()] = CASSymbolRef{uint64_t(Ref.get())};
  }
  return CASSymbolRef{uint64_t(Ref.get())};
}

Optional<CASSymbolRef>
NestedV1ObjectReader::getCASSymbolRefByName(StringRef Name) const {
  std::lock_guard<sys::Mutex> Guard(SymbolsLock);
  auto SymI = SymbolsByName.find(Name);
  if (SymI == SymbolsByName.end())
    return None;
  return SymI->second;
}

Expected<CASSymbol>
ExternalSymbolNodeRef::materialize(const NestedV1ObjectReader &) const {
  StringRef Name = SymbolName.getName();
  CASSymbol Info{Name,  /*Offset=*/0, Linkage, jitlink::Scope::Default,
                 false, false,        false,   None};
  return Info;
}

Expected<CASBlockRef> NestedV1ObjectReader::getOrCreateCASBlockRef(
    const DefinedSymbolNodeRef &ForSymbol, Expected<BlockRef> Block,
    Optional<CASSymbolRef> KeptAliveBySymbol, bool MergeByContent) const {
  if (!Block)
    return Block.takeError();

  /// Whether the produced CASBlockRef can be shared across multiple symbols.
  bool CanShareBlockRef = MergeByContent && !Block->hasAbstractBackedge() &&
                          !Block->hasKeepAliveEdge();

  cas::CASID IDKey =
      CanShareBlockRef ? Block->getID() : ForSymbol.Symbol.getID();

  std::lock_guard<sys::Mutex> Guard(BlocksLock);
  auto &Ref = Blocks[IDKey];
  if (!Ref) {
    Optional<CASSymbolRef> ForSymbolInBlock;
    Optional<CASSymbolRef> KeptAliveBySymbolInBlock;
    if (!CanShareBlockRef) {
      ForSymbolInBlock = CASSymbolRef{uint64_t(&ForSymbol)};
      KeptAliveBySymbolInBlock = KeptAliveBySymbol;
    }
    Ref = std::make_unique<BlockNodeRef>(std::move(ForSymbolInBlock),
                                         std::move(*Block),
                                         std::move(KeptAliveBySymbolInBlock));
  }
  return CASBlockRef{uint64_t(Ref.get())};
}

Expected<CASSectionRef> NestedV1ObjectReader::getOrCreateCASSectionRef(
    Expected<SectionRef> Section) const {
  if (!Section)
    return Section.takeError();

  std::lock_guard<sys::Mutex> Guard(SectionsLock);
  auto &Ref = Sections[*Section];
  if (!Ref) {
    Ref = std::make_unique<SectionNodeRef>(std::move(*Section));
  }
  return CASSectionRef{uint64_t(Ref.get())};
}

Expected<CASSymbol>
DefinedSymbolNodeRef::materialize(const NestedV1ObjectReader &Reader) const {
  Expected<Optional<NameRef>> ExpectedName = Symbol.getName();
  if (!ExpectedName)
    return ExpectedName.takeError();

  StringRef Name;
  if (*ExpectedName)
    Name = (**ExpectedName).getName();

  if (Symbol.isSymbolTemplate()) {
    assert(Name.empty() && "Symbol templates cannot have names");
  }

  Expected<SymbolDefinitionRef> Definition = Symbol.getDefinition();
  if (!Definition)
    return Definition.takeError();

  switch (Definition->getKind()) {
  case SymbolDefinitionRef::Alias:
  case SymbolDefinitionRef::IndirectAlias:
    return createStringError(inconvertibleErrorCode(),
                             "LinkGraph does not support aliases yet");

  case SymbolDefinitionRef::Block:
    break;
  }

  // Check whether this symbol can share existing copies of the block.
  SymbolRef::Flags Flags = Symbol.getFlags();
  bool MergeByContent = Flags.Merge & SymbolRef::M_ByContent;

  // FIXME: Maybe go further here and use MergeableBlocks. Or maybe it's not
  // worth it (not ever going to succeed) when we're within a single compile
  // unit.
  // FIXME: With \p MergeByContent == true multiple symbols may share the same
  // \p Block reference but it is not sufficiently communicated via the
  // CASObjectReader interface whether it is a *requirement* that symbols share
  // the same block (as is the case with aliases) or whether the linker *can*
  // use the same block contents if it desires so (e.g. the linker may share the
  // block for a release build but not for a debug build).
  Expected<CASBlockRef> Block = Reader.getOrCreateCASBlockRef(
      *this, BlockRef::get(*Definition), KeptAliveBySymbol, MergeByContent);
  if (!Block)
    return Block.takeError();

  jitlink::Linkage Linkage = (Flags.Merge & SymbolRef::M_ByName)
                                 ? jitlink::Linkage::Weak
                                 : jitlink::Linkage::Strong;
  jitlink::Scope Scope;
  switch (Flags.Scope) {
  case SymbolRef::S_Global:
    Scope = jitlink::Scope::Default;
    break;
  case SymbolRef::S_Hidden:
    Scope = jitlink::Scope::Hidden;
    break;
  case SymbolRef::S_Local:
    Scope = jitlink::Scope::Local;
    break;
  }
  bool IsLive = Flags.DeadStrip == SymbolRef::DS_Never;
  // FIXME: Should we record an `IsCallable` property for the symbol?
  // Maybe not necessary since it could be a property of the section the symbol
  // belongs to.
  bool IsCallable = false;

  return CASSymbol{
      Name,       unsigned(Symbol.getOffset()), Linkage, Scope, IsLive,
      IsCallable, Symbol.isAutoHide(),          *Block};
}

Expected<CASBlock>
BlockNodeRef::materialize(const NestedV1ObjectReader &Reader) const {
  Expected<CASSectionRef> Section =
      Reader.getOrCreateCASSectionRef(Block.getSection());
  if (!Section)
    return Section.takeError();

  // Get the data.
  Expected<BlockDataRef> BlockData = Block.getBlockData();
  if (!BlockData)
    return BlockData.takeError();
  uint64_t Size = BlockData->getSize();
  uint64_t Alignment = BlockData->getAlignment();
  uint64_t AlignmentOffset = BlockData->getAlignmentOffset();
  Optional<StringRef> Content;
  if (!BlockData->isZeroFill()) {
    Content = BlockData->getContent();
    assert(Content && "Block is not zero-fill so it should have data");
    assert(Size == Content->size());
  }

  return CASBlock{Size, Alignment, AlignmentOffset, Content, *Section};
}

Expected<CASSection> SectionNodeRef::materialize() const {
  Expected<NameRef> Name = Section.getName();
  if (!Name)
    return Name.takeError();
  return CASSection{Name->getName(), Section.getMemProt()};
}

Error BlockNodeRef::materializeFixups(
    const NestedV1ObjectReader &Reader,
    function_ref<Error(const CASBlockFixup &)> Callback) const {
  if (!Block.hasEdges())
    return Error::success();

  Expected<FixupList> Fixups = Block.getFixups();
  if (!Fixups)
    return Fixups.takeError();
  Expected<TargetInfoList> TIs = Block.getTargetInfo();
  if (!TIs)
    return TIs.takeError();
  Expected<TargetList> Targets = Block.getTargets();
  if (!Targets)
    return Targets.takeError();

  auto createMismatchError = [&]() {
    size_t NumFixups = std::distance(Fixups->begin(), Fixups->end());
    size_t NumTIs = std::distance(TIs->begin(), TIs->end());
    return createStringError(inconvertibleErrorCode(),
                             "invalid edge-list with mismatched fixups (" +
                                 Twine(NumFixups) + ") and targets (" +
                                 Twine(NumTIs) + ")");
  };

  FixupList::iterator F = Fixups->begin(), FE = Fixups->end();
  TargetInfoList::iterator TI = TIs->begin(), TIE = TIs->end();
  for (; F != FE && TI != TIE; ++F, ++TI) {
    if (TI->Index > Targets->size())
      return createStringError(inconvertibleErrorCode(),
                               "target index too big for target-list");

    // Check for an abstract backedge.
    if (TI->Index == Targets->size()) {
      assert(KeptAliveBySymbol.hasValue());
      CASBlockFixup CASFixup{F->Offset, F->Kind, TI->Addend,
                             *KeptAliveBySymbol};
      if (Error E = Callback(CASFixup))
        return E;
      continue;
    }

    // Pass this block down for KeepAlive edges.
    Expected<TargetRef> Target = Targets->get(TI->Index);
    if (!Target)
      return Target.takeError();

    CASSymbolRef TargetRef;
    if (Target->getKind() == TargetRef::Symbol) {
      Expected<CASSymbolRef> ExpTarget = Reader.getOrCreateCASSymbolRef(
          SymbolRef::get(Target->getSchema(), *Target),
          F->Kind == jitlink::Edge::KeepAlive ? ForSymbol : None);
      if (!ExpTarget)
        return ExpTarget.takeError();
      TargetRef = *ExpTarget;
    } else {
      assert(Target->getKind() == TargetRef::IndirectSymbol);
      Expected<StringRef> ExpectedName = Target->getNameString();
      if (!ExpectedName)
        return ExpectedName.takeError();
      StringRef Name = *ExpectedName;
      TargetRef = Reader.getCASSymbolRefByName(Name).getValue();
    }

    // Pass in this block's KeptAliveByBlock for edges.
    CASBlockFixup CASFixup{F->Offset, F->Kind, TI->Addend,
                           std::move(TargetRef)};
    if (Error E = Callback(CASFixup))
      return E;
  }
  if (F != FE || TI != TIE)
    return createMismatchError();

  return Error::success();
}
