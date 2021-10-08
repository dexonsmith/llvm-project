//===- ObjectFileSchema.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/CASObjectFormat/ObjectFileSchema.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ExecutionEngine/CASObjectFormat/Encoding.h"
#include "llvm/Support/EndianStream.h"

// FIXME: For jitlink::x86_64::writeOperand(). Should use a generic version.
#include "llvm/ExecutionEngine/JITLink/x86_64.h"

// FIXME: For cl::opt. Should thread through or delete.
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace llvm::casobjectformat;

const StringLiteral NameRef::KindString;
const StringLiteral ContentRef::KindString;
const StringLiteral BlockDataRef::KindString;
const StringLiteral TargetListRef::KindString;
const StringLiteral SectionRef::KindString;
const StringLiteral BlockRef::KindString;
const StringLiteral SymbolRef::KindString;
const StringLiteral SymbolTableRef::KindString;
const StringLiteral NameListRef::KindString;
const StringLiteral CompileUnitRef::KindString;

namespace {
class EncodedDataRef : public SpecificRef<EncodedDataRef> {
  using SpecificRefT = SpecificRef<EncodedDataRef>;
  friend class SpecificRef<EncodedDataRef>;

public:
  static constexpr StringLiteral KindString = "cas.o:encoded-data";

  StringRef getEncodedList() const { return getData(); }

  static Expected<EncodedDataRef>
  create(ObjectFileSchema &Schema,
         function_ref<void(SmallVectorImpl<char> &)> Encode);
  static Expected<EncodedDataRef> get(Expected<ObjectFormatNodeRef> Ref);
  static Expected<EncodedDataRef> get(ObjectFileSchema &Schema, cas::CASID ID) {
    return get(Schema.getNode(ID));
  }

private:
  explicit EncodedDataRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};
} // end namespace

const StringLiteral EncodedDataRef::KindString;

Error ObjectFileSchema::fillCache() {
  if (RootNodeTypeID)
    return Error::success();

  Optional<cas::CASID> RootKindID;
  const unsigned Version = 0;
  if (Expected<cas::NodeRef> ExpectedRootKind =
          CAS.createNode(None, "cas.o:schema:" + Twine(Version).str()))
    RootKindID = *ExpectedRootKind;
  else
    return ExpectedRootKind.takeError();

  StringRef AllKindStrings[] = {
      BlockDataRef::KindString,   BlockRef::KindString,
      CompileUnitRef::KindString, ContentRef::KindString,
      EncodedDataRef::KindString, NameListRef::KindString,
      NameRef::KindString,        SectionRef::KindString,
      SymbolRef::KindString,      SymbolTableRef::KindString,
      TargetListRef::KindString,
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

  auto ExpectedTypeID = CAS.createNode(IDs, "cas.o:root");
  if (!ExpectedTypeID)
    return ExpectedTypeID.takeError();
  RootNodeTypeID = *ExpectedTypeID;
  return Error::success();
}

Optional<StringRef> ObjectFileSchema::getKindString(const cas::NodeRef &Node) {
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

bool ObjectFileSchema::isRootNode(const cas::NodeRef &Node) {
  if (Error E = fillCache())
    report_fatal_error(std::move(E));
  assert(RootNodeTypeID);

  if (Node.getNumReferences() < 1)
    return false;
  return Node.getReference(0) == *RootNodeTypeID;
}

bool ObjectFileSchema::isNode(const cas::NodeRef &Node) {
  if (Error E = fillCache())
    report_fatal_error(std::move(E));

  // This is a very weak check!
  return bool(getKindString(Node));
}

Expected<ObjectFormatNodeRef::Builder>
ObjectFormatNodeRef::Builder::startRootNode(ObjectFileSchema &Schema,
                                            StringRef KindString) {
  Builder B(Schema);
  Expected<cas::CASID> Root = Schema.getRootNodeTypeID();
  if (!Root)
    return Root.takeError();
  B.IDs.push_back(*Root);

  if (Error E = B.startNodeImpl(KindString))
    return std::move(E);
  return std::move(B);
}

Error ObjectFormatNodeRef::Builder::startNodeImpl(StringRef KindString) {
  Optional<unsigned char> TypeID = Schema->getKindStringID(KindString);
  if (!TypeID)
    return createStringError(inconvertibleErrorCode(),
                             "invalid object format kind string: " +
                                 KindString);
  Data.push_back(*TypeID);
  return Error::success();
}

Expected<ObjectFormatNodeRef::Builder>
ObjectFormatNodeRef::Builder::startNode(ObjectFileSchema &Schema,
                                        StringRef KindString) {
  Builder B(Schema);
  if (Error E = B.startNodeImpl(KindString))
    return std::move(E);
  return std::move(B);
}

Expected<ObjectFormatNodeRef> ObjectFormatNodeRef::Builder::build() {
  return ObjectFormatNodeRef::get(*Schema, Schema->CAS.createNode(IDs, Data));
}

StringRef ObjectFormatNodeRef::getKindString() const {
  Optional<StringRef> KS = getSchema().getKindString(*this);
  assert(KS && "Expected valid kind string");
  return *KS;
}
Optional<unsigned char>
ObjectFileSchema::getKindStringID(StringRef KindString) {
  if (!RootNodeTypeID) {
    if (Error E = fillCache()) {
      consumeError(std::move(E));
      return None;
    }
  }
  for (auto &I : KindStrings)
    if (I.second == KindString)
      return I.first;
  return None;
}

Expected<cas::CASID> ObjectFileSchema::getRootNodeTypeID() {
  if (!RootNodeTypeID)
    if (Error E = fillCache())
      return std::move(E);
  return *RootNodeTypeID;
}

Expected<ObjectFormatNodeRef>
ObjectFormatNodeRef::get(ObjectFileSchema &Schema, Expected<cas::NodeRef> Ref) {
  if (!Ref)
    return Ref.takeError();
  if (!Schema.isNode(*Ref))
    return createStringError(
        inconvertibleErrorCode(),
        "invalid kind-string for node in object-file-schema");
  return ObjectFormatNodeRef(Schema, *Ref);
}

Expected<EncodedDataRef>
EncodedDataRef::get(Expected<ObjectFormatNodeRef> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences())
    return createStringError(inconvertibleErrorCode(), "corrupt encoded data");

  return EncodedDataRef(*Specific);
}

Expected<EncodedDataRef>
EncodedDataRef::create(ObjectFileSchema &Schema,
                       function_ref<void(SmallVectorImpl<char> &)> Encode) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  Encode(B->Data);
  return get(B->build());
}

Expected<NameRef> NameRef::get(Expected<ObjectFormatNodeRef> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences())
    return createStringError(inconvertibleErrorCode(), "corrupt name");

  return NameRef(*Specific);
}

Expected<NameRef> NameRef::create(ObjectFileSchema &Schema, StringRef Name) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  B->Data.append(Name);
  return get(B->build());
}

Expected<ContentRef> ContentRef::get(Expected<ObjectFormatNodeRef> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences())
    return createStringError(inconvertibleErrorCode(), "corrupt content");

  return ContentRef(*Specific);
}

Expected<ContentRef> ContentRef::create(ObjectFileSchema &Schema,
                                        StringRef Content) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  // FIXME: Should this be aligned?
  B->Data.append(Content);
  return get(B->build());
}

cas::CASID BlockDataRef::getFixupsID() const {
  assert(HasFixups);
  assert(!EmbeddedFixupsSize);
  return getReference(!isZeroFill());
}

Expected<FixupList> BlockDataRef::getFixups() const {
  if (!HasFixups)
    return FixupList("");

  if (EmbeddedFixupsSize)
    return FixupList(getData().take_back(EmbeddedFixupsSize));

  if (Expected<EncodedDataRef> Fixups =
          EncodedDataRef::get(getSchema(), getFixupsID()))
    return FixupList(Fixups->getEncodedList());
  else
    return Fixups.takeError();
}

Expected<BlockDataRef> BlockDataRef::get(Expected<ObjectFormatNodeRef> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences() > 2)
    return createStringError(inconvertibleErrorCode(),
                             "corrupt block data; got " +
                                 Twine(Specific->getNumReferences()) + " refs");

  StringRef Remaining = Specific->getData();
  bool IsZeroFill = Remaining[0];
  Remaining = Remaining.drop_front();

  uint64_t Size;
  uint64_t Alignment;
  uint64_t AlignmentOffset;
  Error E = encoding::consumeVBR8(Remaining, Size);
  if (!E)
    E = encoding::consumeVBR8(Remaining, Alignment);
  if (!E)
    E = encoding::consumeVBR8(Remaining, AlignmentOffset);
  if (E) {
    consumeError(std::move(E));
    return createStringError(inconvertibleErrorCode(), "corrupt block data");
  }
  uint32_t EmbeddedFixupsSize = Remaining.size();
  bool HasFixups =
      EmbeddedFixupsSize || Specific->getNumReferences() > (!IsZeroFill);

  return BlockDataRef(*Specific, Size, Alignment, AlignmentOffset,
                      EmbeddedFixupsSize, IsZeroFill, HasFixups);
}

cl::opt<size_t> MaxEdgesToEmbedInBlock(
    "max-edges-to-embed-in-block",
    cl::desc("Maximum number of edges to embed in a block."), cl::init(8));

Expected<BlockDataRef>
BlockDataRef::createImpl(ObjectFileSchema &Schema, Optional<ContentRef> Content,
                         uint64_t Size, uint64_t Alignment,
                         uint64_t AlignmentOffset, ArrayRef<Fixup> Fixups) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  if (Content)
    B->IDs.push_back(*Content);

  B->Data.push_back((unsigned char)!Content);
  encoding::writeVBR8(Size, B->Data);
  encoding::writeVBR8(Alignment, B->Data);
  encoding::writeVBR8(AlignmentOffset, B->Data);

  if (Fixups.size() > MaxEdgesToEmbedInBlock) {
    auto FixupsRef =
        EncodedDataRef::create(Schema, [Fixups](SmallVectorImpl<char> &Data) {
          FixupList::encode(Fixups, Data);
        });
    if (!FixupsRef)
      return FixupsRef.takeError();
    B->IDs.push_back(*FixupsRef);
  } else if (!Fixups.empty()) {
    FixupList::encode(Fixups, B->Data);
  }

  // FIXME: Streamline again.
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

// FIXME: Copy/pasted from X86AsmBackend::writeNopData.
static void writeX86NopData(raw_ostream &OS, uint64_t Count) {
  static const char Nops[10][11] = {
      // nop
      "\x90",
      // xchg %ax,%ax
      "\x66\x90",
      // nopl (%[re]ax)
      "\x0f\x1f\x00",
      // nopl 0(%[re]ax)
      "\x0f\x1f\x40\x00",
      // nopl 0(%[re]ax,%[re]ax,1)
      "\x0f\x1f\x44\x00\x00",
      // nopw 0(%[re]ax,%[re]ax,1)
      "\x66\x0f\x1f\x44\x00\x00",
      // nopl 0L(%[re]ax)
      "\x0f\x1f\x80\x00\x00\x00\x00",
      // nopl 0L(%[re]ax,%[re]ax,1)
      "\x0f\x1f\x84\x00\x00\x00\x00\x00",
      // nopw 0L(%[re]ax,%[re]ax,1)
      "\x66\x0f\x1f\x84\x00\x00\x00\x00\x00",
      // nopw %cs:0L(%[re]ax,%[re]ax,1)
      "\x66\x2e\x0f\x1f\x84\x00\x00\x00\x00\x00",
  };

  const uint64_t MaxNopLength = 16;

  // Emit as many MaxNopLength NOPs as needed, then emit a NOP of the remaining
  // length.
  do {
    const uint8_t ThisNopLength = (uint8_t)std::min(Count, MaxNopLength);
    const uint8_t Prefixes = ThisNopLength <= 10 ? 0 : ThisNopLength - 10;
    for (uint8_t i = 0; i < Prefixes; i++)
      OS << '\x66';
    const uint8_t Rest = ThisNopLength - Prefixes;
    if (Rest != 0)
      OS.write(Nops[Rest - 1], Rest);
    Count -= ThisNopLength;
  } while (Count != 0);
}

Expected<BlockDataRef> BlockDataRef::create(ObjectFileSchema &Schema,
                                            const jitlink::Block &Block,
                                            ArrayRef<Fixup> Fixups) {
  if (Block.isZeroFill())
    return createZeroFill(Schema, Block.getSize(), Block.getAlignment(),
                          Block.getAlignmentOffset(), Fixups);

  assert(Block.getContent().size() == Block.getSize());
  StringRef Content(Block.getContent().begin(), Block.getSize());

  Optional<SmallString<1024>> BlockData;
  auto initializeBlockData = [&BlockData, Content]() {
    if (BlockData)
      return;
    BlockData.emplace();
    BlockData->append(Content.begin(), Content.end());
  };
  auto getFixupPtr = [&](const Fixup &F) -> char * {
    initializeBlockData();
    return &(*BlockData)[F.Offset];
  };

  if (ZeroFixupsDuringBlockIngestion) {
    // FIXME: Directly accessing jitlink::x86_64::writeOperand() isn't correct.
    // See also the FIXME in CompileUnitRef::create().
    for (Fixup F : Fixups)
      if (F.Kind >= jitlink::Edge::FirstRelocation)
        if (Error E = jitlink::x86_64::writeOperand(F.Kind, 0, getFixupPtr(F)))
          return std::move(E);
  }

  // Append trailing noops to the last block in __TEXT,__text to canonicalize
  // its shape, to avoid noise when a block happens to be last. This is because
  // we don't know the actual size of the symbol, and when it's not last it
  // already has the padding.
  //
  // FIXME: Only do this when reading blocks parsed from Mach-O.
  //
  // FIXME: Probably we want this elsewhere (not just __TEXT,__text).
  if (AppendTrailingNopPaddingDuringBlockIngestion__text &&
      Block.getSection().getName() == "__TEXT,__text" &&
      !isAligned(Align(16), Block.getSize())) {
    initializeBlockData();

    raw_svector_ostream OS(*BlockData);
    writeX86NopData(OS, offsetToAlignment(Block.getSize(), Align(16)));
  }

  // Use the modified content if something was tweaked.
  if (BlockData)
    Content = *BlockData;

  return createContent(Schema, Content, Block.getAlignment(),
                       Block.getAlignmentOffset(), Fixups);
}

Expected<BlockDataRef> BlockDataRef::createZeroFill(ObjectFileSchema &Schema,
                                                    uint64_t Size,
                                                    uint64_t Alignment,
                                                    uint64_t AlignmentOffset,
                                                    ArrayRef<Fixup> Fixups) {
  return createImpl(Schema, None, Size, Alignment, AlignmentOffset, Fixups);
}

Expected<BlockDataRef> BlockDataRef::createContent(ObjectFileSchema &Schema,
                                                   ContentRef Content,
                                                   uint64_t Alignment,
                                                   uint64_t AlignmentOffset,
                                                   ArrayRef<Fixup> Fixups) {
  return createImpl(Schema, Content, Content.getData().size(), Alignment,
                    AlignmentOffset, Fixups);
}

Expected<BlockDataRef> BlockDataRef::createContent(ObjectFileSchema &Schema,
                                                   StringRef Content,
                                                   uint64_t Alignment,
                                                   uint64_t AlignmentOffset,
                                                   ArrayRef<Fixup> Fixups) {
  Expected<ContentRef> Ref = ContentRef::create(Schema, Content);
  if (!Ref)
    return Ref.takeError();

  return createImpl(Schema, *Ref, Content.size(), Alignment, AlignmentOffset,
                    Fixups);
}

void FixupList::encode(ArrayRef<Fixup> Fixups, SmallVectorImpl<char> &Data) {
  // FIXME: Kinds should be numbered in a stable way, not just rely on
  // Edge::Kind.
  for (auto &F : Fixups) {
    Data.push_back(static_cast<unsigned char>(F.Kind));
    encoding::writeVBR8(uint64_t(F.Offset), Data);
  }
}

void FixupList::iterator::decode(bool IsInit) {
  if (Data.empty()) {
    assert((IsInit || F) && "past the end");
    F.reset();
    return;
  }

  unsigned char Kind = Data[0];
  Data = Data.drop_front();

  uint64_t Offset = 0;
  bool ConsumeFailed = errorToBool(encoding::consumeVBR8(Data, Offset));
  assert(!ConsumeFailed && "Cannot decode vbr8");
  (void)ConsumeFailed;

  F.emplace();
  F->Kind = Kind;
  F->Offset = Offset;
}

void TargetInfoList::encode(ArrayRef<TargetInfo> TIs,
                            SmallVectorImpl<char> &Data) {
  for (const TargetInfo &TI : TIs) {
    assert(TI.Index < TIs.size() && "More targets than edges?");
    encoding::writeVBR8(int64_t(TI.Addend), Data);
    encoding::writeVBR8(uint32_t(TI.Index), Data);
  }
}

void TargetInfoList::iterator::decode(bool IsInit) {
  if (Data.empty()) {
    assert((IsInit || TI) && "past the end");
    TI.reset();
    return;
  }

  int64_t Addend = 0;
  bool ConsumeFailed = errorToBool(encoding::consumeVBR8(Data, Addend));
  assert(!ConsumeFailed && "Cannot decode addend");
  (void)ConsumeFailed;

  uint32_t Index = 0;
  ConsumeFailed = errorToBool(encoding::consumeVBR8(Data, Index));
  assert(!ConsumeFailed && "Cannot decode index");
  (void)ConsumeFailed;

  TI.emplace();
  TI->Addend = Addend;
  TI->Index = Index;
}

Expected<Optional<NameRef>> TargetRef::getName() const {
  if (getKind() == IndirectSymbol)
    return NameRef::get(*Schema, ID);
  auto Symbol = SymbolRef::get(*Schema, ID);
  if (!Symbol)
    return Symbol.takeError();
  return Symbol->getName();
}

Expected<TargetRef> TargetRef::get(ObjectFileSchema &Schema, cas::CASID ID,
                                   Optional<Kind> ExpectedKind) {
  auto checkExpectedKind = [&](Kind K, StringRef Name) -> Expected<TargetRef> {
    if (ExpectedKind && *ExpectedKind != K)
      return createStringError(inconvertibleErrorCode(),
                               "unexpected " + Name + " for target");
    return TargetRef(Schema, ID, K);
  };

  auto Object = ObjectFormatNodeRef::get(Schema, Schema.CAS.getNode(ID));
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
SymbolDefinitionRef::get(Expected<ObjectFormatNodeRef> Ref,
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

Expected<TargetListRef> TargetListRef::get(Expected<ObjectFormatNodeRef> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (!Specific->getData().empty())
    return createStringError(inconvertibleErrorCode(), "corrupt target list");
  return TargetListRef(*Specific);
}

Expected<TargetListRef> TargetListRef::create(ObjectFileSchema &Schema,
                                              ArrayRef<TargetRef> Targets) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  B->IDs.append(Targets.begin(), Targets.end());
  return get(B->build());
}

namespace {
enum SectionProtectionFlags {
  Read = 1,
  Write = 2,
  Exec = 4,
};
}

static sys::Memory::ProtectionFlags
decodeProtectionFlags(SectionProtectionFlags Perms) {
  return sys::Memory::ProtectionFlags(
      (Perms & Read ? sys::Memory::MF_READ : 0) |
      (Perms & Write ? sys::Memory::MF_WRITE : 0) |
      (Perms & Exec ? sys::Memory::MF_EXEC : 0));
}

static SectionProtectionFlags
encodeProtectionFlags(sys::Memory::ProtectionFlags Perms) {
  return SectionProtectionFlags((Perms & sys::Memory::MF_READ ? Read : 0) |
                                (Perms & sys::Memory::MF_WRITE ? Write : 0) |
                                (Perms & sys::Memory::MF_EXEC ? Exec : 0));
}

Expected<SectionRef>
SectionRef::create(ObjectFileSchema &Schema, NameRef SectionName,
                   sys::Memory::ProtectionFlags Protections) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  B->IDs.push_back(SectionName);

  // FIXME: Does 1 byte leave enough space for expansion? Probably, but
  // 4 bytes would be fine too.
  B->Data.push_back(uint8_t(encodeProtectionFlags(Protections)));
  return get(B->build());
}

sys::Memory::ProtectionFlags SectionRef::getProtectionFlags() const {
  return decodeProtectionFlags((SectionProtectionFlags)getData()[0]);
}

Expected<SectionRef> SectionRef::get(Expected<ObjectFormatNodeRef> Ref) {
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
  return getReference(2U + unsigned(Flags.HasTargets));
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
          TargetListRef::get(getSchema(), getReference(*TargetsIndex)))
    return Targets->getTargets();
  else
    return Targets.takeError();
}

Expected<SectionRef> SectionRef::create(ObjectFileSchema &Schema,
                                        const jitlink::Section &S) {
  Expected<NameRef> Name = NameRef::create(Schema, S.getName());
  if (!Name)
    return Name.takeError();
  return create(Schema, *Name, S.getProtectionFlags());
}

static bool compareSymbolsBySemanticsAnd(
    const jitlink::Symbol *LHS, const jitlink::Symbol *RHS,
    function_ref<bool(const jitlink::Symbol *, const jitlink::Symbol *)>
        NextCompare) {
  if (LHS == RHS)
    return false;

  // Sort by name, putting anonymous symbols last.
  if (LHS->hasName() != RHS->hasName())
    return LHS->hasName() > RHS->hasName();
  if (LHS->hasName())
    if (int Diff = LHS->getName().compare(RHS->getName()))
      return Diff < 0;

  // Put external symbols last, stopping if both are external.
  if (LHS->isExternal() != RHS->isExternal())
    return LHS->isExternal() < RHS->isExternal();
  if (LHS->isExternal())
    return NextCompare(LHS, RHS);

  // Put absolute symbols after defined ones. Sort by symbol size if they're
  // both absolute.
  if (LHS->isAbsolute() != RHS->isAbsolute())
    return LHS->isAbsolute() < RHS->isAbsolute();
  if (LHS->isAbsolute()) {
    if (LHS->getSize() != RHS->getSize())
      return LHS->getSize() < RHS->getSize();
    return NextCompare(LHS, RHS);
  }

  // Only defined symbols should remain.
  assert(LHS->isDefined() && "Expected defined symbol");
  assert(RHS->isDefined() && "Expected defined symbol");

  // Compare section name.
  const jitlink::Block &LB = LHS->getBlock();
  const jitlink::Block &RB = RHS->getBlock();
  if (&LB.getSection() != &RB.getSection())
    if (int Diff = LB.getSection().getName().compare(RB.getSection().getName()))
      return Diff < 0;

  // Compare symbol size.
  if (LHS->getSize() != RHS->getSize())
    return LHS->getSize() < RHS->getSize();

  // If it's the same block, compare by symbol offset.
  if (&LB == &RB) {
    if (LHS->getOffset() != RHS->getOffset())
      return LHS->getOffset() < RHS->getOffset();
    return NextCompare(LHS, RHS);
  }

  // Sort structurally by the block.
  if (LB.edges_size() != RB.edges_size())
    return LB.edges_size() < RB.edges_size();
  if (LB.getSize() != RB.getSize())
    return LB.getSize() < RB.getSize();

  // Compare block content.
  if (LB.isZeroFill() != RB.isZeroFill())
    return LB.isZeroFill() < RB.isZeroFill();
  if (LB.isZeroFill())
    return NextCompare(LHS, RHS);

  // FIXME: This could expensive. Maybe this should only be done sometimes
  // (when symbols are mergeable by content?).
  //
  // FIXME: Fixups have not been zeroed out yet so this isn't going to match
  // across TUs.
  if (int Diff = StringRef(LB.getContent().begin(), LB.getSize())
                     .compare(StringRef(RB.getContent().begin(), RB.getSize())))
    return Diff < 0;
  return NextCompare(LHS, RHS);
}

static bool compareSymbolsBySemantics(const jitlink::Symbol *LHS,
                                      const jitlink::Symbol *RHS) {
  return compareSymbolsBySemanticsAnd(
      LHS, RHS,
      [](const jitlink::Symbol *, const jitlink::Symbol *) { return false; });
}

static bool compareSymbolsByLinkageAndSemantics(const jitlink::Symbol *LHS,
                                                const jitlink::Symbol *RHS) {
  if (LHS == RHS)
    return false;

  // Put locals last.
  if (LHS->getScope() != RHS->getScope())
    return LHS->getScope() < RHS->getScope();

  // Put strong symbols before weak symbols.
  if (LHS->getLinkage() != RHS->getLinkage())
    return LHS->getLinkage() < RHS->getLinkage();

  // Put no-dead-strip symbols ahead of others.
  if (LHS->isLive() != RHS->isLive())
    return LHS->isLive() > RHS->isLive();

  return compareSymbolsBySemantics(LHS, RHS);
}

static bool compareSymbolsByAddress(const jitlink::Symbol *LHS,
                                    const jitlink::Symbol *RHS) {
  if (LHS == RHS)
    return false;

  if (LHS->isExternal() != RHS->isExternal())
    return LHS->isExternal() < RHS->isExternal();

  JITTargetAddress LAddr = LHS->getAddress();
  JITTargetAddress RAddr = RHS->getAddress();
  if (LAddr != RAddr)
    return LAddr < RAddr;

  return LHS->getSize() < RHS->getSize();
}

static Error decomposeAndSortEdges(
    const jitlink::Block &Block, SmallVectorImpl<Fixup> &Fixups,
    SmallVectorImpl<TargetInfo> &TIs, SmallVectorImpl<TargetRef> &Targets,
    function_ref<Expected<Optional<TargetRef>>(
        const jitlink::Symbol &, jitlink::Edge::Kind, bool IsFromData,
        jitlink::Edge::AddendT &Addend, Optional<StringRef> &SplitContent)>
        GetTargetRef) {
  assert(Fixups.empty());
  assert(TIs.empty());
  assert(Targets.empty());
  if (!Block.edges_size())
    return Error::success();

  // Guess whether the edges are coming from data or code.
  //
  // FIXME: This isn't robust. Data can be stored in executable sections. We
  // differentiation in the edge kind instead.
  bool IsFromData =
      !(Block.getSection().getProtectionFlags() & sys::Memory::MF_EXEC);

  // Collect edges and targets, filtering out duplicate symbols.
  SmallVector<const jitlink::Edge *, 16> Edges;
  Edges.reserve(Block.edges_size());
  struct TargetAndSymbol {
    TargetRef Target;
    const jitlink::Symbol *Symbol;
    Optional<StringRef> SplitContent;
  };
  struct EdgeTarget {
    Optional<TargetRef> Target; // None if this is an abstract backedge.
    jitlink::Edge::AddendT Addend = ~0U;
  };
  bool HasAbstractBackedge = false;
  SmallDenseMap<cas::CASID, size_t, 16> SymbolIndex;
  SmallDenseMap<const jitlink::Edge *, EdgeTarget, 16> EdgeTargets;
  SmallVector<TargetAndSymbol, 16> TargetData;
  for (const jitlink::Edge &E : Block.edges()) {
    Edges.push_back(&E);
    const jitlink::Symbol &S = E.getTarget();

    Optional<StringRef> SplitContent;
    jitlink::Edge::AddendT Addend = E.getAddend();
    Expected<Optional<TargetRef>> TargetOrAbstractBackedge =
        GetTargetRef(S, E.getKind(), IsFromData, Addend, SplitContent);
    if (!TargetOrAbstractBackedge)
      return TargetOrAbstractBackedge.takeError();

    // Map edges to targets.
    EdgeTargets.insert(
        std::make_pair(&E, EdgeTarget{*TargetOrAbstractBackedge, Addend}));
    if (!*TargetOrAbstractBackedge) {
      HasAbstractBackedge = true;
      continue;
    }

    // Unique the targets themselves.
    TargetRef Target = **TargetOrAbstractBackedge;
    if (!SymbolIndex.insert(std::make_pair(Target.getID(), ~0U)).second)
      continue;
    TargetData.push_back(TargetAndSymbol{Target, &S, SplitContent});
  }
  assert(!Edges.empty() && "No edges inserted?");
  assert(EdgeTargets.size() == Edges.size());
  assert((HasAbstractBackedge || !TargetData.empty()) &&
         "No symbols inserted?");
  assert(TargetData.size() == SymbolIndex.size());

  // Make the order of targets stable and fill in the index map.
  std::stable_sort(TargetData.begin(), TargetData.end(),
                   [](const TargetAndSymbol &LHS, const TargetAndSymbol &RHS) {
                     if (LHS.Symbol != RHS.Symbol)
                       return compareSymbolsBySemanticsAnd(
                           LHS.Symbol, RHS.Symbol, compareSymbolsByAddress);

                     // Same symbol but different target means that either it's
                     // a different kind of reference, or a block got broken up.
                     if (LHS.Target.getKind() != RHS.Target.getKind())
                       return LHS.Target.getKind() < RHS.Target.getKind();

                     // Check the contents that were split out.
                     if (LHS.SplitContent && !RHS.SplitContent)
                       return true;
                     if (!LHS.SplitContent && RHS.SplitContent)
                       return false;
                     if (LHS.SplitContent)
                       if (int Diff =
                               LHS.SplitContent->compare(*RHS.SplitContent))
                         return Diff < 0;

                     // Give up.
                     return false;
                   });

  for (size_t I = 0, E = TargetData.size(); I != E; ++I)
    SymbolIndex[TargetData[I].Target] = I;
  assert(TargetData.size() == SymbolIndex.size() &&
         "No symbols should have been added by lookup!");

  // Sort the edges.
  std::stable_sort(Edges.begin(), Edges.end(),
                   [&SymbolIndex, &EdgeTargets](const jitlink::Edge *LHS,
                                                const jitlink::Edge *RHS) {
                     // Compare the fixup.
                     if (LHS->getOffset() < RHS->getOffset())
                       return true;
                     if (RHS->getOffset() < LHS->getOffset())
                       return false;
                     if (LHS->getKind() < RHS->getKind())
                       return true;
                     if (RHS->getKind() < LHS->getKind())
                       return false;

                     // Compare the target and addend. In practice this is
                     // likely dead code.
                     const EdgeTarget &LHSTarget =
                         EdgeTargets.find(LHS)->second;
                     const EdgeTarget &RHSTarget =
                         EdgeTargets.find(RHS)->second;

                     // Abstract backedges implicitly have an index past the
                     // end of the target list. Return them last.
                     bool IsLHSAbstract = !LHSTarget.Target;
                     bool IsRHSAbstract = !RHSTarget.Target;
                     if (IsLHSAbstract || IsRHSAbstract)
                       return IsLHSAbstract < IsRHSAbstract;

                     assert(!IsLHSAbstract);
                     assert(!IsRHSAbstract);
                     size_t LHSIndex = SymbolIndex.lookup(*LHSTarget.Target);
                     size_t RHSIndex = SymbolIndex.lookup(*RHSTarget.Target);
                     if (LHSIndex < RHSIndex)
                       return true;
                     if (RHSIndex < LHSIndex)
                       return false;
                     return EdgeTargets.find(LHS)->second.Addend <
                            EdgeTargets.find(RHS)->second.Addend;
                   });
  assert(TargetData.size() == SymbolIndex.size() &&
         "No symbols should have been added by lookup!");

  // Fill in the addends and target indices.
  Fixups.reserve(Edges.size());
  TIs.reserve(Edges.size());
  Optional<size_t> AbstractBackedgeIndex;
  if (HasAbstractBackedge)
    AbstractBackedgeIndex = TargetData.size();
  for (const jitlink::Edge *E : Edges) {
    Fixups.emplace_back();
    Fixups.back().Kind = E->getKind();
    Fixups.back().Offset = E->getOffset();

    EdgeTarget &ET = EdgeTargets.find(E)->second;
    TIs.emplace_back();
    TIs.back().Index =
        ET.Target ? SymbolIndex.lookup(*ET.Target) : *AbstractBackedgeIndex;
    TIs.back().Addend = ET.Addend;
  }
  assert(TargetData.size() == SymbolIndex.size() &&
         "No symbols should have been added by lookup!");

  // Copy out the targets.
  Targets.reserve(TargetData.size());
  for (const auto &I : TargetData)
    Targets.push_back(I.Target);

  return Error::success();
}

Expected<BlockRef> BlockRef::create(
    ObjectFileSchema &Schema, const jitlink::Block &Block,
    function_ref<Expected<Optional<TargetRef>>(
        const jitlink::Symbol &, jitlink::Edge::Kind, bool IsFromData,
        jitlink::Edge::AddendT &Addend, Optional<StringRef> &SplitContent)>
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
  return createImpl(Schema, *Section, *Data, TIs, Targets);
}

cl::opt<bool>
    InlineUnaryTargetLists("inline-unary-target-lists",
                           cl::desc("Whether to inline unary target lists."),
                           cl::init(true));

Expected<BlockRef> BlockRef::createImpl(ObjectFileSchema &Schema,
                                        SectionRef Section, BlockDataRef Data,
                                        ArrayRef<TargetInfo> TargetInfo,
                                        ArrayRef<TargetRef> Targets) {
  Expected<Builder> B = Builder::startNode(Schema, KindString);
  if (!B)
    return B.takeError();

  B->IDs.append({Section, Data});

  bool HasAbstractBackedge = false;
  for (const auto &TI : TargetInfo) {
    assert(TI.Index <= Targets.size() && "Target index out of range");
    HasAbstractBackedge |= TI.Index == Targets.size();
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

Expected<BlockRef> BlockRef::get(Expected<ObjectFormatNodeRef> Ref) {
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

Expected<SymbolRef> SymbolRef::get(Expected<ObjectFormatNodeRef> Ref) {
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

  bool IsLiveSection = false;
  if (S.isDefined()) {
    StringRef Name = S.getBlock().getSection().getName();

    // FIXME: Stop using hardcoded section names.
    if (KeepStaticInitializersAlive)
      IsLiveSection |= Name == "__TEXT,__constructor";
    if (KeepCompactUnwindAlive)
      IsLiveSection |= Name == "__LD,__compact_unwind";
  }

  // FIXME: Can we detect DS_CompileUnit in more cases?
  F.DeadStrip = (S.isLive() || IsLiveSection)
                    ? DS_Never
                    : (IsAutoHide || (UseDeadStripCompileForLocals &&
                                      S.getScope() == jitlink::Scope::Local))
                          ? DS_CompileUnit
                          : DS_LinkUnit;

  return F;
}

Expected<SymbolRef> SymbolRef::create(
    ObjectFileSchema &Schema, const jitlink::Symbol &S,
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

Expected<SymbolRef> SymbolRef::create(ObjectFileSchema &Schema,
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
CompileUnitRef::get(Expected<ObjectFormatNodeRef> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (Specific->getNumReferences() != 7)
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
    ObjectFileSchema &Schema, const Triple &TT, unsigned PointerSize,
    support::endianness Endianness, SymbolTableRef DeadStripNever,
    SymbolTableRef DeadStripLink, SymbolTableRef IndirectDeadStripCompile,
    SymbolTableRef IndirectAnonymous, NameListRef StrongSymbols,
    NameListRef WeakSymbols) {
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
                 IndirectAnonymous, StrongSymbols, WeakSymbols});
  return get(B->build());
}

Expected<NameListRef> NameListRef::create(ObjectFileSchema &Schema,
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

Expected<NameListRef> NameListRef::get(Expected<ObjectFormatNodeRef> Ref) {
  auto Specific = SpecificRefT::getSpecific(std::move(Ref));
  if (!Specific)
    return Specific.takeError();

  if (!Specific->getData().empty())
    return createStringError(inconvertibleErrorCode(), "corrupt name list");

  return NameListRef(*Specific);
}

Expected<SymbolTableRef> SymbolTableRef::create(ObjectFileSchema &Schema,
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
SymbolTableRef::get(Expected<ObjectFormatNodeRef> Ref) {
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
}

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
  ObjectFileSchema &Schema;
  raw_ostream *DebugOS;

  CompileUnitBuilder(ObjectFileSchema &Schema, raw_ostream *DebugOS)
      : CAS(Schema.CAS), Schema(Schema), DebugOS(DebugOS) {}

  struct SymbolInfo {
    Optional<NameRef> Indirect;
    Optional<SymbolRef> Symbol;
    ArrayRef<const jitlink::Symbol *> NestedDeadStripCompile;
  };
  DenseMap<const jitlink::Symbol *, SymbolInfo> Symbols;
  DenseMap<const jitlink::Block *, BlockRef> Blocks;

  SmallVector<SymbolRef> DeadStripNeverSymbols;
  SmallVector<SymbolRef> DeadStripLinkSymbols;
  SmallVector<SymbolRef> IndirectAnonymousSymbols;

  // Goal state: first few references in symbol-table are other symbol-tables
  // (only used by IndirectDeadLinkCompile). Each nested table contains at
  // least one symbol, and some number of nested ones.
  //
  // Call graph of:
  //
  //     e1
  //     f1->f2->f3->f4
  //     g1->g2->g4
  //       ->g3->g4
  //           ->g5
  //       ->g4
  //       ->g5
  //     h1->f2
  //       ->g2
  //       ->h2
  //
  // (where all of these belong in IndirectDeadLinkCompile) would result in the
  // following:
  //
  //     symbol-table: IndirectDeadStripCompile
  //       symbol-table: f1
  //         symbol-table: f2
  //           symbol-table: f3
  //             symbol: f3
  //             symbol: f4
  //           symbol: f2
  //         symbol: f1
  //       symbol-table: g1
  //         symbol-table: g2
  //           symbol: g2
  //           symbol: g4
  //         symbol-table: g3
  //           symbol: g3
  //           symbol: g4
  //           symbol: g5
  //         symbol: g1
  //       symbol-table: h1
  //         symbol-table: f2 (same object as above)
  //         symbol-table: g2 (same object as above)
  //         symbol: h1
  //         symbol: h2
  //       symbol: e1
  //
  // Note that 'g4' and 'g5' are skipped from the symbol-table for 'g1' since
  // they are stored in the nested table(s). But 'g4' needs to be in both 'g2'
  // and 'g3' because neither is nested in the other.
  //
  // These nested symbol tables can be deduped within a build when another
  // object file calls a subset of the functions (e.g., file2.o might call
  // f2 directly, and can share f2's symbol table). They can be deduped across
  // builds when the calling functions change without changing the called
  // functions (e.g., f1 changes but f2/f3/f4 remain the same).
  //
  // Algorithm:
  //
  //  1. While defining symbols, build a graph of references between these
  //     nestable symbols. The graph should be a DAG, just using targets that
  //     would be direct if not for PreferIndirectSymbolRefs.
  //  2. Post-order traversal of graph, building symbol tables for nodes that
  //     directly reference others. Exclude the direct references that are also
  //     transitive by traversing parent graph to see if there's another route
  //     (or by building sets of transitive nodes for each thing).
  //  3. Top-level table works as-if a "root" node had references to all
  //     symbols in the table.
  //  4. CompileUnitRef points at the top-level table.
  //
  // Then the LinkGraphBuilder needs to do the inverse.
  SpecificBumpPtrAlloc<const jitlink::Symbol *> NestedDeadStripCompileAlloc;
  SmallVector<const jitlink::Symbol *> IndirectDeadStripCompileSymbols;

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
  getOrCreateTarget(const jitlink::Symbol &S, const jitlink::Block &SourceBlock,
                    jitlink::Edge::Kind K, bool IsFromData,
                    jitlink::Edge::AddendT &Addend,
                    Optional<StringRef> &SplitContent);

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
               compareSymbolsByAddress);
  };
  for (const jitlink::Section &Section : G.sections())
    appendSymbols(Section.symbols());
  appendSymbols(G.absolute_symbols());

  // Sort all symbols in an address-independent way to prepare for visiting
  // them. Use a stable sort so that the previous sort-by-section-then-address
  // will break ties.
  std::stable_sort(Symbols.begin(), Symbols.end(),
                   compareSymbolsByLinkageAndSemantics);

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
    DeadStripNeverSymbols.push_back(*Symbol);
    break;
  case SymbolRef::DS_LinkUnit:
    DeadStripLinkSymbols.push_back(*Symbol);
    break;
  case SymbolRef::DS_CompileUnit:
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
    //
    // FIXME: This will currently have ALL linkonce_odr+autohide functions in
    // it, because they're all referenced indirectly by their __eh_frame.
    if (HasIndirectReference)
      IndirectDeadStripCompileSymbols.push_back(*Symbol);
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

  Expected<BlockRef> ExpectedBlock = BlockRef::create(
      Schema, B,
      [&](const jitlink::Symbol &S, jitlink::Edge::Kind K, bool IsFromData,
          jitlink::Edge::AddendT &Addend, Optional<StringRef> &SplitContent) {
        return getOrCreateTarget(S, B, K, IsFromData, Addend, SplitContent);
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

Expected<Optional<TargetRef>> CompileUnitBuilder::getOrCreateTarget(
    const jitlink::Symbol &S, const jitlink::Block &SourceBlock,
    jitlink::Edge::Kind K, bool IsFromData, jitlink::Edge::AddendT &Addend,
    Optional<StringRef> &SplitContent) {
  // Use an abstract target for the back-edge from FDEs in __TEXT,__eh_frame.
  if (UseAbstractBackedgeForPCBeginInFDEDuringBlockIngestion &&
      isPCBeginFromFDE(S, SourceBlock))
    return None;

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

Expected<CompileUnitRef> CompileUnitRef::create(ObjectFileSchema &Schema,
                                                const jitlink::LinkGraph &G,
                                                raw_ostream *DebugOS) {
  if (!G.getTargetTriple().isX86()) {
    assert(G.getTargetTriple().isX86() &&
           "FIXME: Requires x86_64 for access to writeOperand()");
    return createStringError(inconvertibleErrorCode(),
                             "target triple must be x86_64 for now");
  }

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

  return CompileUnitRef::create(Schema, G.getTargetTriple(), G.getPointerSize(),
                                G.getEndianness(), *NeverTable, *LinkTable,
                                *CompileTable, *AnonymousTable,
                                *StrongExternals, *WeakExternals);
}

namespace {
/// Builder for eagerly building a LinkGraph, visiting each top-level symbol
/// in turn.
///
/// - worklist: symbols that have been created without a definition.
/// - getOrCreateSymbol:
///    - If the symbol exists, return it.
///    - Else, create it with no definition and push onto the worklist.
/// - getOrCreateBlock:
///    - if such a block exists, returns it
///    - call getOrCreateSymbol for each target
///    - create and return the block
/// - createOrDuplicateBlock:
///    - call getOrCreateBlock, then duplicate it
/// - defineSymbol:
///    - generates a definition:
///       - getOrCreateBlock for `Merge & ByContent`
///       - createOrDuplicateBlock for `!(Merge & ByContent)`
///       - getOrCreateSymbol for an alias
///    - calls LinkGraph::makeDefined
///
/// General algorithm: walk through top-level symbol tables, calling
/// getOrCreateSymbol on each symbol, then running through the worklist until
/// empty.
class LinkGraphBuilder {
public:
  struct SymbolToDefine {
    jitlink::Symbol *S;
    SymbolRef Ref;
    jitlink::Symbol *KeptAliveBySymbol;
  };

  struct DelayedEdge {
    jitlink::Block *FromBlock;
    jitlink::Edge::Kind K;
    jitlink::Edge::OffsetT Offset;
    jitlink::Block *KeptAliveBySymbol;
    jitlink::Edge::AddendT Addend;
  };

  Error makeExternalSymbols(Expected<NameListRef> ExternalSymbols,
                            jitlink::Linkage Linkage);
  Error makeExternalSymbol(ObjectFileSchema &Schema,
                           Expected<NameRef> SymbolName,
                           jitlink::Linkage Linkage);
  Error makeSymbols(Expected<SymbolTableRef> Table);
  Error makeSymbol(Expected<SymbolRef> Symbol);

  /// Create a symbol for \p Target.
  ///
  /// - If \p AddAbstractEdge is set, it should be used for adding abstract
  /// backedges.
  /// - If \p KeptAliveByBlock is set, this is a keep-alive edge.
  Expected<jitlink::Symbol *>
  getOrCreateSymbol(Expected<TargetRef> Target,
                    jitlink::Symbol *KeptAliveBySymbol);
  Expected<jitlink::Block *>
  getOrCreateBlock(jitlink::Symbol &ForSymbol, BlockRef Block,
                   jitlink::Symbol *KeptAliveBySymbol);
  Expected<jitlink::Block *>
  createOrDuplicateBlock(jitlink::Symbol &ForSymbol, BlockRef Block,
                         jitlink::Symbol *KeptAliveBySymbol);
  Error defineSymbol(SymbolToDefine Symbol);

  using AddAbstractBackedgeT = llvm::function_ref<void(
      jitlink::Edge::Kind, jitlink::Edge::OffsetT, jitlink::Edge::AddendT)>;

  /// Returns true if the KeptAliveByBlock is used, indicating the block must
  /// be cached based on the block. Otherwise returns false (or error).
  Error addEdges(jitlink::Symbol &ForSymbol, jitlink::Block &B, BlockRef Block,
                 AddAbstractBackedgeT AddAbstractBackedge);

  LinkGraphBuilder(jitlink::LinkGraph &G) : G(G) {}

  jitlink::LinkGraph &G;

  template <class RefT> struct TypedID {
    cas::CASID ID;
    operator cas::CASID() const { return ID; }
    TypedID(RefT Ref) : ID(Ref.getID()) {}

  private:
    friend struct DenseMapInfo<TypedID>;
    explicit TypedID(cas::CASID ID) : ID(ID) {}
  };

  using SymbolID = TypedID<SymbolRef>;
  using TargetID = TypedID<TargetRef>;
  using SectionID = TypedID<SectionRef>;
  using BlockID = TypedID<BlockRef>;
  using BlockDataID = TypedID<BlockDataRef>;
  using TargetListID = TypedID<TargetListRef>;

  /// Used for de-duping block instantiations.
  ///
  /// FIXME: This isn't useful here, except as a sketch. A single compile unit
  /// is unlikely to have many hits. Instead, this should be used by a
  /// lazy-loading linker algorithm that wants to de-dup blocks by content.
  struct MergeableBlock {
    // FIXME: Since the section and data can be used directly for merging,
    // maybe the format should split them out.
    SectionID Section;
    BlockDataID Data;

    /// Realized targets. Pointing at the same array as \a TargetLists
    /// (allocated in \a TargetListAlloc).
    ArrayRef<jitlink::Symbol *> TargetList;

    friend bool operator==(const MergeableBlock &LHS,
                           const MergeableBlock &RHS) {
      return LHS.Section.ID == RHS.Section.ID && LHS.Data.ID == RHS.Data.ID &&
             LHS.TargetList == RHS.TargetList;
    }
  };

  struct MergeableBlockInfo {
    static bool isEqual(const MergeableBlock *LHS, const MergeableBlock *RHS) {
      assert(LHS != getTombstoneKey());
      assert(LHS != getEmptyKey());
      assert(LHS != nullptr);
      return isEqual(*LHS, RHS);
    }
    static bool isEqual(const MergeableBlock &LHS, const MergeableBlock *RHS) {
      assert(RHS != getTombstoneKey());
      assert(RHS != getEmptyKey());
      assert(RHS != nullptr);
      return LHS == *RHS;
    }
    static unsigned getHashValue(const MergeableBlock &MB) {
      return hash_combine(
          hash_value(MB.Section.ID.getHash()), hash_value(MB.Data.ID.getHash()),
          hash_value(MB.TargetList));
    }

    static unsigned getHashValue(const MergeableBlock *MB) {
      assert(MB != getTombstoneKey());
      assert(MB != getEmptyKey());
      assert(MB != nullptr);
      return getHashValue(*MB);
    }
    static MergeableBlock *getTombstoneKey() {
      return DenseMapInfo<MergeableBlock *>::getTombstoneKey();
    }
    static MergeableBlock *getEmptyKey() {
      return DenseMapInfo<MergeableBlock *>::getEmptyKey();
    }
  };

  // Allocators.
  SpecificBumpPtrAllocator<jitlink::Symbol *> TargetListAlloc;
  SpecificBumpPtrAllocator<MergeableBlock> MergeableBlockAlloc;

  /// Work list of symbols to define.
  SmallVector<SymbolToDefine> Worklist;

  /// Declared sections.
  struct SectionInfo {
    jitlink::Section *Section;
    uint64_t Size = 0;
    uint64_t Alignment = 1;
  };
  DenseMap<SectionID, SectionInfo> Sections;

  /// Declared symbols.
  DenseMap<TargetID, jitlink::Symbol *> Symbols;

  /// Symbols indexed by name.
  DenseMap<StringRef, jitlink::Symbol *> SymbolsByName;

  /// Resolved target lists.
  DenseMap<TargetListID, ArrayRef<jitlink::Symbol *>> TargetLists;

  /// A block. For now, assume its existing symbols do not have
  /// Merge=ByContent, in which case only symbols that can be merged are added.
  /// This is asymmetric in the case that a mergeable reference to a block is
  /// found first.
  DenseMap<BlockID, jitlink::Block *> Blocks;

  /// Merging blocks by content.
  ///
  /// FIXME: This is more of a sketch. Probably not useful for building a
  /// LinkGraph from a single object file, but can be used as a de-duping
  /// algorithm for merging blocks during a link.
  DenseSet<MergeableBlock *> MergeableBlocks;

  /// Track the block used for forward-declaring symbols.
  jitlink::Block *ForwardDeclaredBlock = nullptr;
};
} // namespace

namespace llvm {
template <class RefT>
struct DenseMapInfo<LinkGraphBuilder::TypedID<RefT>>
    : public DenseMapInfo<cas::CASID> {
  static LinkGraphBuilder::TypedID<RefT> getEmptyKey() {
    return LinkGraphBuilder::TypedID<RefT>{
        DenseMapInfo<cas::CASID>::getEmptyKey()};
  }

  static LinkGraphBuilder::TypedID<RefT> getTombstoneKey() {
    return LinkGraphBuilder::TypedID<RefT>{
        DenseMapInfo<cas::CASID>::getTombstoneKey()};
  }
};
} // namespace llvm

Expected<std::unique_ptr<jitlink::LinkGraph>> CompileUnitRef::createLinkGraph(
    StringRef Name,
    jitlink::LinkGraph::GetEdgeKindNameFunction GetEdgeKindName) {
  auto G = std::make_unique<jitlink::LinkGraph>(Name.str(), TT, PointerSize,
                                                Endianness, GetEdgeKindName);

  LinkGraphBuilder Builder(*G);
  if (Error E = Builder.makeExternalSymbols(getStrongExternals(),
                                            jitlink::Linkage::Strong))
    return std::move(E);
  if (Error E = Builder.makeExternalSymbols(getWeakExternals(),
                                            jitlink::Linkage::Weak))
    return std::move(E);
  if (Error E = Builder.makeSymbols(getIndirectAnonymous()))
    return std::move(E);
  if (Error E = Builder.makeSymbols(getIndirectDeadStripCompile()))
    return std::move(E);
  if (Error E = Builder.makeSymbols(getDeadStripLink()))
    return std::move(E);
  if (Error E = Builder.makeSymbols(getDeadStripNever()))
    return std::move(E);

  // Return early if there were no forward declarations.
  if (!Builder.ForwardDeclaredBlock)
    return std::move(G);

  // Clean up the block used for forward declarations.
  //
  // FIXME: It'd be good to remove the now-empty section as well.
  jitlink::Section &ForwardDeclaredSection =
      Builder.ForwardDeclaredBlock->getSection();
  if (ForwardDeclaredSection.symbols_size()) {
    assert(Builder.Worklist.empty());
    return createStringError(
        inconvertibleErrorCode(),
        "Definition not found for forward declared symbol");
  }
  G->removeBlock(*Builder.ForwardDeclaredBlock);
  assert(!ForwardDeclaredSection.blocks_size() &&
         "Expected only the now-deleted forward-declaration block");

  // Fix up addresses.
  uint64_t SectionAddress = 0;
  DenseMap<jitlink::Section *, LinkGraphBuilder::SectionInfo *> AddressInfo;
  for (auto &I : Builder.Sections)
    AddressInfo[I.second.Section] = &I.second;
  for (jitlink::Section &Section : G->sections()) {
    LinkGraphBuilder::SectionInfo *Info = AddressInfo.lookup(&Section);
    if (!Info) {
      assert(&Section == &ForwardDeclaredSection);
      continue;
    }
    SectionAddress = alignTo(SectionAddress, Info->Alignment);

    for (jitlink::Block *B : Section.blocks())
      B->setAddress(B->getAddress() + SectionAddress);

    SectionAddress += Info->Size;
  }

  return std::move(G);
}

Error LinkGraphBuilder::makeExternalSymbols(
    Expected<NameListRef> ExternalSymbols, jitlink::Linkage Linkage) {
  assert(!ForwardDeclaredBlock &&
         "Expected no forward-declared symbols when creating externals");
  if (!ExternalSymbols)
    return ExternalSymbols.takeError();
  for (size_t I = 0, E = ExternalSymbols->getNumNames(); I != E; ++I)
    if (Error E = makeExternalSymbol(ExternalSymbols->getSchema(),
                                     ExternalSymbols->getName(I), Linkage))
      return E;
  return Error::success();
}

Error LinkGraphBuilder::makeSymbols(Expected<SymbolTableRef> Table) {
  if (!Table)
    return Table.takeError();
  for (size_t I = 0, E = Table->getNumSymbols(); I != E; ++I)
    if (Error E = makeSymbol(Table->getSymbol(I)))
      return E;
  return Error::success();
}

Error LinkGraphBuilder::makeExternalSymbol(ObjectFileSchema &Schema,
                                           Expected<NameRef> SymbolName,
                                           jitlink::Linkage Linkage) {
  if (!SymbolName)
    return SymbolName.takeError();
  TargetRef Target = TargetRef::getIndirectSymbol(Schema, *SymbolName);
  StringRef Name = SymbolName->getName();

  jitlink::Symbol &S = G.addExternalSymbol(Name, /*Size=*/0, Linkage);
  Symbols[Target] = &S;
  SymbolsByName[Name] = &S;
  return Error::success();
}

Error LinkGraphBuilder::makeSymbol(Expected<SymbolRef> Symbol) {
  if (!Symbol)
    return Symbol.takeError();
  jitlink::Symbol *S = Symbols.lookup(Symbol->getAsTarget());
  if (!S) {
    if (Expected<jitlink::Symbol *> ExpectedS =
            getOrCreateSymbol(Symbol->getAsTarget(), nullptr))
      S = *ExpectedS;
    else
      return ExpectedS.takeError();
  }
  // FIXME: Complete a post-order traversal of each symbol/block graph before
  // going through the worklist, thus creating jitlink::Block instances
  // bottom-up. This will make the LinkGraph address assignment more
  // predictable, and also will fix __eh_frame to have the CIEs defined ahead
  // of FDEs.
  //
  // FIXME: Handle abstract backedges somehow more directly (likely as part of
  // the above).
  while (!Worklist.empty())
    if (Error E = defineSymbol(Worklist.pop_back_val()))
      return E;
  return Error::success();
}

Expected<jitlink::Symbol *>
LinkGraphBuilder::getOrCreateSymbol(Expected<TargetRef> Target,
                                    jitlink::Symbol *KeptAliveBySymbol) {
  if (!Target)
    return Target.takeError();
  if (jitlink::Symbol *S = Symbols.lookup(*Target))
    return S;

  Optional<StringRef> Name;
  Optional<SymbolRef> Symbol;
  if (Target->getKind() == TargetRef::Symbol) {
    Expected<SymbolRef> ExpectedSymbol =
        SymbolRef::get(Target->getSchema(), *Target);
    if (!ExpectedSymbol)
      return ExpectedSymbol.takeError();
    Symbol = *ExpectedSymbol;
    Expected<Optional<NameRef>> ExpectedName = Symbol->getName();
    if (!ExpectedName)
      return ExpectedName.takeError();

    if (*ExpectedName)
      Name = (**ExpectedName).getName();

    if (Symbol->isSymbolTemplate()) {
      assert(!Name && "Symbol templates cannot have names");
    }
  } else {
    assert(Target->getKind() == TargetRef::IndirectSymbol);
    Expected<StringRef> ExpectedName = Target->getNameString();
    if (!ExpectedName)
      return ExpectedName.takeError();
    Name = *ExpectedName;
  }

  // Check the name table if the symbol could have been referenced by a
  // different type of target.
  jitlink::Symbol *S = Name ? SymbolsByName.lookup(*Name) : nullptr;
  if (!S) {
    // Forward declare and fix it up later.
    if (!ForwardDeclaredBlock) {
      jitlink::Section &Section = G.createSection(
          "cas.o: forward-declared", sys::Memory::ProtectionFlags{});
      ForwardDeclaredBlock = &G.createZeroFillBlock(Section, 1, 1, 1, 0);
    }
    S = Name ? &G.addDefinedSymbol(*ForwardDeclaredBlock, 0, *Name, 0,
                                   jitlink::Linkage::Strong,
                                   jitlink::Scope::Local, false, false)
             : &G.addAnonymousSymbol(*ForwardDeclaredBlock, 0, 0, false, false);
  }

  if (Symbol)
    Worklist.push_back(SymbolToDefine{S, *Symbol, KeptAliveBySymbol});
  if (!Symbol || !Symbol->isSymbolTemplate())
    Symbols[*Target] = S;
  if (Name)
    SymbolsByName[*Name] = S;
  return S;
}

static uint64_t getAlignedAddress(LinkGraphBuilder::SectionInfo &Section,
                                  uint64_t Size, uint64_t Alignment,
                                  uint64_t AlignmentOffset) {
  uint64_t Address = alignTo(Section.Size + AlignmentOffset, Align(Alignment)) -
                     AlignmentOffset;
  Section.Size = Address + Size;
  if (Alignment > Section.Alignment)
    Section.Alignment = Alignment;
  return Address;
}

Expected<jitlink::Block *>
LinkGraphBuilder::getOrCreateBlock(jitlink::Symbol &ForSymbol, BlockRef Block,
                                   jitlink::Symbol *KeptAliveBySymbol) {
  if (jitlink::Block *B = Blocks.lookup(Block))
    return B;

  // Get the section.
  Expected<SectionRef> Section = Block.getSection();
  if (!Section)
    return Section.takeError();
  SectionInfo &S = Sections[*Section];
  if (!S.Section) {
    Expected<NameRef> Name = Section->getName();
    if (!Name)
      return Name.takeError();
    S.Section =
        &G.createSection(Name->getName(), Section->getProtectionFlags());
  }

  // Get the data.
  Expected<BlockDataRef> BlockData = Block.getBlockData();
  if (!BlockData)
    return BlockData.takeError();
  uint64_t Size = BlockData->getSize();
  uint64_t Alignment = BlockData->getAlignment();
  uint64_t AlignmentOffset = BlockData->getAlignmentOffset();
  Optional<ArrayRef<char>> Content;
  if (!BlockData->isZeroFill()) {
    Expected<Optional<ContentRef>> ExpectedContent = BlockData->getContent();
    if (!ExpectedContent)
      return ExpectedContent.takeError();
    assert(*ExpectedContent && "Block is not zero-fill so it should have data");
    Content = (*ExpectedContent)->getContentArray();
    assert(Size == Content->size());
  }

  uint64_t Address = getAlignedAddress(S, Size, Alignment, AlignmentOffset);
  jitlink::Block &B = Content
                          ? G.createContentBlock(*S.Section, *Content, Address,
                                                 Alignment, AlignmentOffset)
                          : G.createZeroFillBlock(*S.Section, Size, Address,
                                                  Alignment, AlignmentOffset);

  // Set the edges.
  bool HasAbstractBackedge = false;
  if (Error E =
          addEdges(ForSymbol, B, Block,
                   [&](jitlink::Edge::Kind K, jitlink::Edge::OffsetT Offset,
                       jitlink::Edge::AddendT Addend) {
                     HasAbstractBackedge = true;
                     assert(KeptAliveBySymbol);
                     B.addEdge(K, Offset, *KeptAliveBySymbol, Addend);
                   }))
    return std::move(E);

  // Cannot / should not cache this if it has an abstract backedge.
  if (!HasAbstractBackedge)
    Blocks[Block] = &B;
  return &B;
}

Error LinkGraphBuilder::addEdges(jitlink::Symbol &ForSymbol, jitlink::Block &B,
                                 BlockRef Block,
                                 AddAbstractBackedgeT AddAbstractBackedge) {
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
      assert(AddAbstractBackedge);
      AddAbstractBackedge(F->Kind, F->Offset, TI->Addend);
      continue;
    }

    // Pass this block down for KeepAlive edges.
    Expected<jitlink::Symbol *> Target = getOrCreateSymbol(
        Targets->get(TI->Index),
        F->Kind == jitlink::Edge::KeepAlive ? &ForSymbol : nullptr);
    if (!Target)
      return Target.takeError();

    // Pass in this block's KeptAliveByBlock for edges.
    assert(*Target && "Expected non-null symbol for target");
    B.addEdge(F->Kind, F->Offset, **Target, TI->Addend);
  }
  if (F != FE || TI != TIE)
    return createMismatchError();

  return Error::success();
}

Expected<jitlink::Block *>
LinkGraphBuilder::createOrDuplicateBlock(jitlink::Symbol &ForSymbol,
                                         BlockRef Block,
                                         jitlink::Symbol *KeptAliveBySymbol) {
  jitlink::Block *B = Blocks.lookup(Block);
  if (!B)
    return getOrCreateBlock(ForSymbol, Block, KeptAliveBySymbol);

  SectionInfo &S = Sections[cantFail(Block.getSection())];
  assert(S.Section == &B->getSection());
  uint64_t Address = getAlignedAddress(S, B->getSize(), B->getAlignment(),
                                       B->getAlignmentOffset());
  return B->isZeroFill()
             ? &G.createZeroFillBlock(B->getSection(), B->getSize(), Address,
                                      B->getAlignment(),
                                      B->getAlignmentOffset())
             : &G.createContentBlock(B->getSection(), B->getContent(), Address,
                                     B->getAlignment(),
                                     B->getAlignmentOffset());
}

Error LinkGraphBuilder::defineSymbol(SymbolToDefine Symbol) {
  assert(&Symbol.S->getBlock() == ForwardDeclaredBlock);

  Expected<SymbolDefinitionRef> Definition = Symbol.Ref.getDefinition();
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

  Expected<BlockRef> DefinitionBlock = BlockRef::get(*Definition);
  if (!DefinitionBlock)
    return DefinitionBlock.takeError();

  // Check whether this symbol can share existing copies of the block.
  SymbolRef::Flags Flags = Symbol.Ref.getFlags();
  bool MergeByContent = Flags.Merge & SymbolRef::M_ByContent;

  // FIXME: Maybe go further here and use MergeableBlocks. Or maybe it's not
  // worth it (not ever going to succeed) when we're within a single compile
  // unit.
  Expected<jitlink::Block *> Block =
      MergeByContent ? getOrCreateBlock(*Symbol.S, *DefinitionBlock,
                                        Symbol.KeptAliveBySymbol)
                     : createOrDuplicateBlock(*Symbol.S, *DefinitionBlock,
                                              Symbol.KeptAliveBySymbol);
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

  G.redefineSymbol(*Symbol.S, **Block, Symbol.Ref.getOffset(),
                   /*Size=*/0, Linkage, Scope, IsLive);
  return Error::success();
}
