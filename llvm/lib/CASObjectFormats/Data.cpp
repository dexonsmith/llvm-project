//===- CASObjectFormats/Data.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CASObjectFormats/Data.h"
#include "llvm/CASObjectFormats/Encoding.h"
#include "llvm/ExecutionEngine/JITLink/MemoryFlags.h"
#include "llvm/Support/EndianStream.h"

using namespace llvm;
using namespace llvm::casobjectformats;
using namespace llvm::casobjectformats::data;

jitlink::MemProt data::decodeProtectionFlags(SectionProtectionFlags Perms) {
  return (Perms & Read ? jitlink::MemProt::Read : jitlink::MemProt::None) |
         (Perms & Write ? jitlink::MemProt::Write : jitlink::MemProt::None) |
         (Perms & Exec ? jitlink::MemProt::Exec : jitlink::MemProt::None);
}

SectionProtectionFlags data::encodeProtectionFlags(jitlink::MemProt Perms) {
  return SectionProtectionFlags(
      ((Perms & jitlink::MemProt::Read) != jitlink::MemProt::None ? Read : 0) |
      ((Perms & jitlink::MemProt::Write) != jitlink::MemProt::None ? Write
                                                                   : 0) |
      ((Perms & jitlink::MemProt::Exec) != jitlink::MemProt::None ? Exec : 0));
}

bool SymbolAttributes::isValid() const {
  // Check conflicting flags for Exported and Undefined.
  if (Flags & (isExported() ? SymbolFlags::IllegalFlagsIfExported
                            : SymbolFlags::ExportedFlags))
      return false;
  if (Flags & (isUndefined() ? SymbolFlags::IllegalFlagsIfUndefined
                              : SymbolFlags::UndefinedFlags))
    return false;

  // The extra bit for GlobalUnnamedAddr should only be set with UnnamedAddr.
  if (!isUnnamedAddress() && (Flags & SymbolFlags::UnnamedAddressFlags))
    return false;

  // Visibility flags are mutually exclusive.
  if (Flags & SymbolFlags::VisibilityFlags) {
    assert((isHidden() || isProtected()) &&
            "Expected to enumerate visibility flags...");
    if (isHidden() && isProtected())
      return false;
  }

  // Check the bit for Autohide isn't used in isolation
  if (!isAutohide() && (Flags & SymbolFlags::AutohideFlag))
    return false;

  return true;
}

constexpr size_t SymbolAttributes::EncodedSize;

SymbolAttributes SymbolAttributes::get(const jitlink::Symbol &Symbol) {
  Optional<data::SymbolAttributes> Attrs;
  if (Symbol.isExternal()) {
    Attrs = Symbol.getLinkage() == jitlink::Linkage::Weak
                ? data::SymbolAttributes::getUndefinedOrNull()
                : data::SymbolAttributes::getUndefined();
  } else {
    Optional<data::Scope> S;
    switch (Symbol.getScope()) {
    case jitlink::Scope::Default:
      S = data::Scope::Global;
      break;
    case jitlink::Scope::Hidden:
      S = data::Scope::Hidden;
      break;
    case jitlink::Scope::Local:
      S = data::Scope::Local;
      break;
    }
    Attrs.emplace(*S);

    // FIXME: Investigate whether it's valid for LinkGraph::addCommonSymbol()
    // to be called with jitlink::Scope::Local; more likely, this
    // "isExported()" check can be skipped and the test in
    // NestedV1SchemaTest::BlockSymbols should be changed instead (and maybe an
    // assertion added to LinkGraph?).
    if (Attrs->isExported())
      if (Symbol.getLinkage() == jitlink::Linkage::Weak)
        Attrs->setLinkage(Linkage::Weak);
  }

  if (Symbol.isLive())
    Attrs->setKeepAlive(KeepAlive::Always);

  // Fine-tune various settings for Mach-O's ".weak_def_can_be_hidden".
  if (Symbol.isAutoHide())
    Attrs->setAutoHide();

  return *Attrs;
}

void SymbolAttributes::print(raw_ostream &OS) const {
  OS << getScope();
  if (isUndefined())
    return;

  auto printAttr = [&](StringRef Prefix, auto V) {
    if (V != decltype(V)::Default)
      OS << '+' << Prefix << V;
  };
  printAttr("", getHiding());
  printAttr("", getLinkage());
  printAttr("keep=", getKeepAlive());
  printAttr("noaddr=", getUnnamedAddress());
}

LLVM_DUMP_METHOD void SymbolAttributes::dump() const { print(dbgs()); }

void SymbolAttributes::encode(SmallVectorImpl<char> &Data) const {
  unsigned Data = unsigned(SA.getKeepAlive()) << 0 |
                  unsigned(SA.getUnnamedAddress()) << 2 |
                  unsigned(SA.getScope()) << 4 |
                  unsigned(SA.getLinkage()) << 7 |
                  unsigned(SA.getHiding()) << 8;
  SymbolAttributesEncodingT Encoded = Data;
  assert(Encoded == Data && "Ran out of bits");

  raw_svector_ostream OS(Data);
  support::endian::Writer EW(OS, support::endianness::little);
  EW.write(encodeSymbolAttributes(*this));
}

static Expected<SymbolAttributes>
decodeSymbolAttributes(SymbolAttributesEncodedT Data) {
  if (Data == SA_UndefinedValue)
    return SymbolAttributes::getUndefined();
  if (Data == SA_UndefinedOrNullValue)
    return SymbolAttributes::getUndefinedOrNull();

  auto makeError = [Data]() {
    return createStringError(inconvertibleErrorCode(),
                             "invalid symbol attributes '" +
                                 Twine((unsigned long long)Data) + "'");
  };

  // Check if bits reserved for undefined attributes are corrupt.
  if ((Data & SA_IsUndefinedBits) == SA_IsUndefinedBits)
    return makeError();

  auto extract = [Data](SymbolAttributesEncodedT Mask, int Start,
                        unsigned Max) {
    unsigned Extracted = (Data & Mask) >> Start;

    // Check logic for masks and shifting. This should pass even if there is
    // data corruption.
    assert(Extracted < Max && "Logic error in encoding");
    return Extracted;
  };
#define SA_EXTRACT_MACRO(T, M) T(extract(SA_##T##Mask, SA_##T##Start, M))
  auto K = SA_EXTRACT_MACRO(KeepAlive, 4);
  auto U = SA_EXTRACT_MACRO(UnnamedAddress, 4);
  auto S = SA_EXTRACT_MACRO(Scope, 4);
  auto L = SA_EXTRACT_MACRO(Linkage, 2);
  auto H = SA_EXTRACT_MACRO(Hiding, 2);
#undef SA_EXTRACT_MACRO

  // Check for corrupt data before constructing.
  if (U > UnnamedAddress::Max || K > KeepAlive::Max)
    return makeError();
  if (S == Scope::Local && !(L == Linkage::Default && H == Hiding::Default))
    return makeError();

  return SymbolAttributes(S, H, L, K, U);
}

Expected<SymbolAttributes> SymbolAttributes::decode(StringRef Data) {
  if (Data.size() != EncodedSize)
    return createStringError(inconvertibleErrorCode(),
                             "invalid symbol attributes");
  using namespace llvm::support;
  auto Bits = endian::read<SymbolAttributesEncodedT, endianness::little, unaligned>(Data.begin());
  return decodeSymbolAttributes(Bits);
}

Expected<SymbolAttributes> SymbolAttributes::consume(StringRef &Data) {
  if (Data.size() < EncodedSize)
    return createStringError(inconvertibleErrorCode(),
                             "invalid symbol attributes");
  Expected<SymbolAttributes> Decoded = decode(Data.take_front(EncodedSize));
  if (Decoded)
    Data = Data.drop_front(EncodedSize);
  return Decoded;
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

void BlockData::encode(uint64_t Size, uint64_t Alignment,
                       uint64_t AlignmentOffset, Optional<StringRef> Content,
                       ArrayRef<Fixup> Fixups, SmallVectorImpl<char> &Data) {
  assert(!Content || Size == Content->size() && "Mismatched content size");
  assert(Alignment && "Expected non-zero alignment");
  assert(isPowerOf2_64(Alignment) && "Expected alignment to be a power of 2");
  assert(AlignmentOffset < Alignment &&
         "Expected alignment offset to be less than alignment");

  // Value of 63 means alignment of 2^62, which is really big. We can steal some
  // bits.
  const unsigned EncodedAlignment = llvm::encode(Align(Alignment));
  assert(EncodedAlignment > 0 && "Expected alignment to be non-zero");
  assert(EncodedAlignment < (1 << 6) &&
         "Expected alignment to leave room for 2 bits");

  const bool HasAlignmentOffset = AlignmentOffset;
  const bool IsZeroFill = !Content;
  Data.push_back(EncodedAlignment |
                 (unsigned(HasAlignmentOffset) << HasAlignmentOffsetBit) |
                 (unsigned(IsZeroFill) << IsZeroFillBit));

  encoding::writeVBR8(Size, Data);
  if (Content)
    Data.append(Content->begin(), Content->end());
  FixupList::encode(Fixups, Data);

  if (!HasAlignmentOffset)
    return;

  raw_svector_ostream OS(Data);
  support::endian::Writer EW(OS, support::endianness::little);

  const size_t NumBytes = getNumAlignmentOffsetBytes(Alignment);
  switch (NumBytes) {
  default:
    llvm_unreachable("invalid alignment?");
  case 1:
    EW.write(uint8_t(AlignmentOffset));
    break;
  case 2:
    EW.write(uint16_t(AlignmentOffset));
    break;
  case 4:
    EW.write(uint32_t(AlignmentOffset));
    break;
  case 8:
    EW.write(uint64_t(AlignmentOffset));
    break;
  }
}

Error BlockData::decode(uint64_t &Size, uint64_t &Alignment,
                        uint64_t &AlignmentOffset, Optional<StringRef> &Content,
                        FixupList &Fixups) const {
  // Reset everything to start.
  Size = Alignment = AlignmentOffset = 0;
  Content = None;
  Fixups = FixupList("");

  // First byte.
  Alignment = getAlignment();
  const bool IsZeroFill = isZeroFill();
  const bool HasAlignmentOffset = hasAlignmentOffset();

  // Remaining.
  StringRef Remaining = Data.drop_front(AfterHeader);
  if (Error E = consumeSize(Remaining, Size))
    return E;
  if (!IsZeroFill)
    if (Error E = consumeContent(Remaining, Size, Content))
      return E;
  if (HasAlignmentOffset) {
    AlignmentOffset = decodeAlignmentOffset();
    Remaining = Remaining.drop_back(getNumAlignmentOffsetBytes(Alignment));
  }
  Fixups = FixupList(Remaining);
  return Error::success();
}

Error BlockData::consumeSize(StringRef &Remaining, uint64_t &Size) {
  return encoding::consumeVBR8(Remaining, Size);
}

uint64_t BlockData::consumeSizeFatal(StringRef &Remaining) {
  uint64_t Size;
  if (Error E = encoding::consumeVBR8(Remaining, Size))
    report_fatal_error(std::move(E));
  return Size;
}

Error BlockData::consumeContent(StringRef &Remaining, uint64_t Size,
                                Optional<StringRef> &Content) {
  if (Remaining.size() < Size)
    return createStringError(inconvertibleErrorCode(), "corrupt block data");
  Content = Remaining.take_front(Size);
  Remaining = Remaining.drop_front(Size);
  return Error::success();
}

Optional<StringRef> BlockData::getContent() const {
  if (isZeroFill())
    return None;

  StringRef Remaining = Data.drop_front(AfterHeader);
  uint64_t Size = consumeSizeFatal(Remaining);
  Optional<StringRef> Content;
  if (Error E = consumeContent(Remaining, Size, Content))
    report_fatal_error(std::move(E));
  return Content;
}

FixupList BlockData::getFixups() const {
  StringRef Remaining = Data.drop_front(AfterHeader);
  uint64_t Size = consumeSizeFatal(Remaining);
  if (!isZeroFill()) {
    assert(Remaining.size() >= Size && "Expected content");
    Remaining = Remaining.drop_front(Size);
  }
  if (hasAlignmentOffset()) {
    size_t NumBytes = getNumAlignmentOffsetBytes(getAlignment());
    assert(Remaining.size() >= NumBytes && "Expected content");
    Remaining = Remaining.drop_back(NumBytes);
  }
  return FixupList(Remaining);
}

uint64_t BlockData::decodeAlignmentOffset() const {
  assert(hasAlignmentOffset() && "Expected to have an alignment offset");

  using namespace llvm::support;
  const size_t NumBytes = getNumAlignmentOffsetBytes(getAlignment());
  assert(NumBytes < Data.size() && "Alignment offset too big?");
  const char *Start = Data.end() - NumBytes;
  switch (NumBytes) {
  default:
    llvm_unreachable("invalid alignment?");
  case 1:
    return endian::read<uint8_t, endianness::little, unaligned>(Start);
  case 2:
    return endian::read<uint16_t, endianness::little, unaligned>(Start);
  case 4:
    return endian::read<uint32_t, endianness::little, unaligned>(Start);
  case 8:
    return endian::read<uint64_t, endianness::little, unaligned>(Start);
  }
}

void TargetInfo::print(raw_ostream &OS) const {
  OS << "{addend=" << Addend << ",index=" << Index << "}";
}

LLVM_DUMP_METHOD void TargetInfo::dump() const { print(dbgs()); }

void TargetInfoList::encode(ArrayRef<TargetInfo> TIs,
                            SmallVectorImpl<char> &Data) {
  for (const TargetInfo &TI : TIs) {
    bool HasAddend = TI.Addend;
    uint64_t IndexAndHasAddend = (uint64_t(TI.Index) << 1) | HasAddend;
    encoding::writeVBR8(IndexAndHasAddend, Data);
    if (HasAddend)
      encoding::writeVBR8(TI.Addend, Data);
  }
}

void TargetInfoList::iterator::decode(bool IsInit) {
  if (Data.empty()) {
    assert((IsInit || TI) && "past the end");
    TI.reset();
    return;
  }

  uint64_t IndexAndHasAddend = 0;
  {
    bool ConsumeFailed = errorToBool(encoding::consumeVBR8(Data,
                                                           IndexAndHasAddend));
    assert(!ConsumeFailed && "Cannot decode index");
    (void)ConsumeFailed;
  }

  int64_t Addend = 0;
  if (IndexAndHasAddend & 0x1) {
    bool ConsumeFailed = errorToBool(encoding::consumeVBR8(Data, Addend));
    assert(!ConsumeFailed && "Cannot decode addend");
    (void)ConsumeFailed;
  }

  TI.emplace();
  TI->Addend = Addend;
  TI->Index = IndexAndHasAddend >> 1;
}

#define CASE_SA_PRINT_ENUM(V, S)                                               \
  V:                                                                           \
  OS << S;                                                                     \
  break
raw_ostream &data::operator<<(raw_ostream &OS, const Scope &S) {
  switch (S) {
  case CASE_SA_PRINT_ENUM(Scope::Undefined, "undef");
  case CASE_SA_PRINT_ENUM(Scope::UndefinedOrNull, "undef_or_null");
  case CASE_SA_PRINT_ENUM(Scope::Local, "local");
  case CASE_SA_PRINT_ENUM(Scope::Global, "global");
  case CASE_SA_PRINT_ENUM(Scope::Protected, "protected");
  case CASE_SA_PRINT_ENUM(Scope::Hidden, "hidden");
  }
  return OS;
}

raw_ostream &data::operator<<(raw_ostream &OS, const Hiding &H) {
  switch (H) {
  case CASE_SA_PRINT_ENUM(Hiding::Default, "default");
  case CASE_SA_PRINT_ENUM(Hiding::Hideable, "hideable");
  }
  return OS;
}

raw_ostream &data::operator<<(raw_ostream &OS, const Linkage &L) {
  switch (L) {
  case CASE_SA_PRINT_ENUM(Linkage::Default, "default");
  case CASE_SA_PRINT_ENUM(Linkage::Weak, "weak");
  }
  return OS;
}

raw_ostream &data::operator<<(raw_ostream &OS, const KeepAlive &K) {
  switch (K) {
  case CASE_SA_PRINT_ENUM(KeepAlive::Default, "none");
  case CASE_SA_PRINT_ENUM(KeepAlive::Referenced, "referenced");
  case CASE_SA_PRINT_ENUM(KeepAlive::Always, "always");
  }
  return OS;
}

raw_ostream &data::operator<<(raw_ostream &OS, const UnnamedAddress &U) {
  switch (U) {
  case CASE_SA_PRINT_ENUM(UnnamedAddress::Default, "default");
  case CASE_SA_PRINT_ENUM(UnnamedAddress::Local, "local");
  case CASE_SA_PRINT_ENUM(UnnamedAddress::Global, "global");
  }
  return OS;
}
#undef CASE_SA_PRINT_ENUM
