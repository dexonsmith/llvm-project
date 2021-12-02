//===- llvm/CASObjectFormats/Data.h -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CASOBJECTFORMATS_DATA_H
#define LLVM_CASOBJECTFORMATS_DATA_H

#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/Support/Alignment.h"
#include <type_traits>

namespace llvm {

class raw_ostream;

namespace casobjectformats {
namespace data {

/// Stable enum to model parts of sys::Memory::ProtectionFlags relevant for
/// sections in object files.
enum SectionProtectionFlags {
  Read = 1,
  Write = 2,
  Exec = 4,
};

jitlink::MemProt decodeProtectionFlags(SectionProtectionFlags Perms);

SectionProtectionFlags encodeProtectionFlags(jitlink::MemProt Perms);

/// The scope of the variable.
enum class Scope {
  /// Not exported. Only visibile within a compile unit.
  ///
  /// LLVM IR: "default" visibility; "internal" or "private".
  Local,

  /// Exported from the compile unit, but not the linkage unit. Can be
  /// referenced from other compile units.
  ///
  /// The link effectively downgrades to \a Local by not including this in the
  /// dynamic symbol table.
  ///
  /// LLVM IR: "hidden" visibility.
  Hidden,

  /// Exported from the linkage unit. Can be referenced from other linkage
  /// units.
  ///
  /// LLVM IR: "default" visibility; not "internal" or "private".
  /// "linkonce_odr".
  Global,

  /// Exported from the linkage unit. Can be referenced from other linkage
  /// units. Cannot be replaced by other linkage units.
  ///
  /// LLVM IR: "protected" visibility.
  Protected,

  /// Undefined / externally defined.
  ///
  /// LLVM IR: "external" linkage.
  Undefined,

  UndefinedOrNull,

  /// Convenience for data validation.
  Max = UndefinedOrNull,
};
raw_ostream &operator<<(raw_ostream &OS, const Scope &S);

enum class SymbolFlags : unsigned char {
  Default = 0,

  /// Exported. May be referenced by other compile units (and barring other
  /// flags, other linkage units).
  ///
  /// Requires: !Undefined.
  ///
  /// LLVM IR: not "internal" or "private".
  Exported = 1,

  /// Weak linkage: one definition is selected at link time.
  ///
  /// LLVM IR: "common", "weak", "weak_odr", "linkonce", and "linkonce_odr".
  Weak = Exported | 1U << 1,

  /// Exported from the compile unit, but not the linkage unit. Can be
  /// referenced from other compile units.
  ///
  /// The link effectively downgrades to \a Local by not including this in the
  /// dynamic symbol table.
  ///
  /// LLVM IR: "hidden" visibility.
  Hidden = Exported | 1U << 2,

  /// Exported, but cannot be overridden. Only relevant for ELF.
  ///
  /// LLVM IR: "protected" visibility.
  Protected = Exported | 1U << 3,

  /// "One definition rule". Language guarantee that other exported symbols
  /// with the same name are semantically equivalent.
  ///
  /// LLVM IR: "weak_odr" and "linkonce_odr".
  ODR = Exported | 1U << 4,
  WeakODR = Weak | ODR,

  /// Discardable despite being exported, unless also \a Used.
  ///
  /// LLVM IR: "linkonce" and "linkonce_odr". "available_externally" also
  /// matches this, although LLVM always discards it before lowering.
  Discardable = Exported | 1U << 5,
  Linkonce = Weak | Discardable,
  LinkonceODR = Linkonce | Discardable,

  /// Bits that depend on \a Exported.
  ExportedFlags = Exported | Weak | Hidden | ODR | Discardable,

  /// Address is not taken for comparison locally. If exported, it's possible
  /// that another compile unit or linkage unit takes its address and compares
  /// it.
  ///
  /// If/when the scope is \a Scope::Local (or becomes so; e.g., after hiding
  /// from symbol table), this can legally alias a symbol that is known to have
  /// equivalent content.
  ///
  /// LLVM IR: "local_unnamed_addr" and "unnamed_addr".
  LocalUnnamedAddress = 1U << 6,

  /// Can be removed from the symbol table (downgraded to \a Scope::Local)
  /// without changing program semantics, even if it cannot be discarded
  /// due to references.
  ///
  /// LLVM IR: "linkonce_odr" with "unnamed_addr" or when constant with
  /// "local_unnamed_addr".
  Autohide = LinkonceODR | LocalUnnamedAddress,

  /// Callable.
  ///
  /// LLVM IR: function, ifuncs, and aliases of functions and ifuncs.
  Callable = 1U << 7,

  /// Undefined / externally defined.
  ///
  /// Requires: !Exported.
  ///
  /// LLVM IR: declarations ("declare").
  Undefined = 1U << 8

  /// Undefined / externally defined, or possibly null.
  ///
  /// LLVM IR: "extern_weak" linkage.
  ExternWeak = Undefined | 1U << 9,

  /// Bits that depend on \a Undefined. No other bit is valid if any of these
  /// is set.
  UndefinedFlags = Undefined | ExternWeak,

  /// Flag to indicate this is a template in the object format encoding. This
  /// helps to improve deduplication of EH frame symbols in the nestedv1
  /// format where symbols currently reference their transitive call graph, by
  /// using a placeholder edge to the function that gets fixed up later using
  /// the function's keep-alive edge to the EH frame.
  ///
  /// Requires: !Exported.
  ///
  /// FIXME: Stop relying on this and remove it.
  EncodingTemplate = 1U << 10,

  /// Flag to indicate that the definition has an outgoing edge that has
  /// "inverted" liveness semantics. Such an edge means that instead of the
  /// source (this symbol) keeping the target alive, the target keeps alive the
  /// source, and if the target is discarded then this should be too.
  ///
  /// This allows metadata content (such an FDR in EH frames, or a slice of
  /// debug info specific to a function) to be kept alive by the thing it
  /// describes *without* modifying it.
  ///
  /// Requires: !Exported.
  ///
  /// TODO: Add an "inverted" liveness property to edges (so this flag is EVER
  /// true) and stop serializing \a jitlink::Edge::KeepAlive edges in \a
  /// jitlink::LinkGraph in favour of serializing these. The KeepAlive edges in
  /// \a jitlink::LinkGraph model this liveness relationship by mutating the
  /// target. The opposite behaviour would leave the target unchanged. It also
  /// avoids adding a cycle, allowing \a EncodingTemplate above to be removed.
  ///
  /// LLVM IR: does not exist (yet!), but would be useful for precisely
  /// modelling liveness of ASan global variable descriptors and Swift protocol
  /// conformances. In LLVM IR, syntax could be "lives_for(...)":
  /// /code
  /// @var = global i32 0
  /// @.var.desc = private {i32*, i32} {i32* @var, i32 7},
  ///              section "specialsection",
  ///              lives_for(@var)
  /// /endcode
  /// ... where @desc should be kept-alive as long as @var is. In other object
  /// file formats, @desc would be lowered as-if in "!llvm.used". But in this
  /// one, its edge to @global would be marked with inverted liveness and the
  /// symbol would have \a HasInvertedLivenessEdge set.
  HasInvertedLivenessEdge = 1U << 11,

  LLVM_MARK_AS_BITMASK_ENUM(/*LargestValue=*/HasInvertedLivenessEdge);
};
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();
raw_ostream &operator<<(raw_ostream &OS, const SymbolFlags &F);

/// Rules for whether a symbol is discardable.
enum class KeepAlive {
  /// Discardable if not referenced and not exported.
  ///
  /// LLVM IR: most symbols not in "!llvm.used".
  Default,

  Referenced,

  Always,

  /// Convenience for data validation.
  Max = Always,
};
raw_ostream &operator<<(raw_ostream &OS, const KeepAlive &K);

/// Symbol attributes.
///
/// Valid combinations depend on the \a Scope. Setters (that assert on invalid)
/// provided for other attributes.
///
/// Encoding guaranteed to be a single byte. In-memory bitfield takes two bytes
/// (for simplicity).
class SymbolAttributes {
public:
  constexpr Scope getScope() const { return Scope(S); }
  constexpr Hiding getHiding() const { return Hiding(H); }
  constexpr Linkage getLinkage() const { return Linkage(L); }
  constexpr KeepAlive getKeepAlive() const { return KeepAlive(K); }
  constexpr UnnamedAddress getUnnamedAddress() const {
    return UnnamedAddress(U);
  }

  void setHiding(Hiding NewH) {
    assert((NewH == Hiding::Default || isExported()) &&
           "Only exported symbols can be hideable");
    H = unsigned(NewH);
  }

  void setLinkage(Linkage NewL) {
    assert((NewL == Linkage::Default || isExported()) &&
           "Only exported symbols can be weak");
    L = unsigned(NewL);
  }

  void setKeepAlive(KeepAlive NewK) {
    assert((NewK == KeepAlive::Default || !isUndefined()) &&
           "Cannot keep alive undefined symbol alive");
    K = unsigned(NewK);
  }

  void setUnnamedAddress(UnnamedAddress NewU) {
    assert((NewU == UnnamedAddress::Default || !isUndefined()) &&
           "Cannot mark undefined symbol as address-not-taken");
    U = unsigned(NewU);
    assert(isValid());
  }

  /// Set the attributes implied by ".weak_def_can_be_hidden".
  void setAutoHide() {
    assert(isExported() && "Can only auto-hide exported symbols");
    setLinkage(Linkage::Weak);
    setHiding(Hiding::Hideable);
    if (getKeepAlive() == KeepAlive::Default)
      setKeepAlive(KeepAlive::Referenced);
    if (getUnnamedAddress() == UnnamedAddress::Default)
      setUnnamedAddress(UnnamedAddress::Local);
  }

  /// Is this symbol undefined / imported / defined elsewhere?
  constexpr bool isUndefined() const {
    return getScope() == Scope::Undefined ||
           getScope() == Scope::UndefinedOrNull;
  }

  /// Is this symbol defined locally and exported for linking?
  constexpr bool isExported() const {
    return getScope() == Scope::Global || getScope() == Scope::Hidden ||
           getScope() == Scope::Protected;
  }

  /// Is this symbol defined locally and not exported?
  constexpr bool isLocal() const { return getScope() == Scope::Local; }

  /// Is this symbol always considered "used" even if not referenced?
  bool isUsed() const { return getKeepAlive() == KeepAlive::Always; }

  /// Is this symbol weak?
  bool isWeak() const { return getLinkage() == Linkage::Weak; }

  /// Should this symbol be treated like a symbol declared with the Mach-O
  /// ".weak_def_can_be_hidden" directive?
  bool isAutoHide() const {
    return getHiding() == Hiding::Hideable && getLinkage() == Linkage::Weak &&
           getUnnamedAddress() >= UnnamedAddress::Local;
  }

  /// Will this symbol be hidden from the symbol table (local) after linking?
  bool isHiddenAfterLink() const { return getScope() <= Scope::Hidden; }

  /// Can this symbol be hidden from the symbol table (made local) after
  /// linking?
  bool isHideableAfterLink() const {
    return isHiddenAfterLink() || getHiding() == Hiding::Hideable;
  }

  /// Can the symbol be discarded if not referenced?
  bool canDiscard() const {
    if (isUsed())
      return false;
    return !isExported() || getKeepAlive() == KeepAlive::Referenced;
  }

  /// Can the symbol be discarded after linking if no longer (or still not)
  /// referenced?
  bool canDiscardAfterLink() const {
    if (isUsed())
      return false;
    return canDiscard() || isHideableAfterLink();
  }

  /// If it points at a constant, is this safe to merge with other constants?
  bool canMergeConstants() const {
    return getUnnamedAddress() >= UnnamedAddress::Local;
  }

  /// Provide a stable sort order.
  int compare(const SymbolAttributes &RHS) const {
#define LLVM_DATA_SYMBOL_ATTRIBUTES_COMPARE_ATTRIBUTE(X)                       \
  if (X != RHS.X)                                                              \
    return X < RHS.X ? -1 : 1;
    LLVM_DATA_SYMBOL_ATTRIBUTES_COMPARE_ATTRIBUTE(S)
    LLVM_DATA_SYMBOL_ATTRIBUTES_COMPARE_ATTRIBUTE(H)
    LLVM_DATA_SYMBOL_ATTRIBUTES_COMPARE_ATTRIBUTE(L)
    LLVM_DATA_SYMBOL_ATTRIBUTES_COMPARE_ATTRIBUTE(K)
    LLVM_DATA_SYMBOL_ATTRIBUTES_COMPARE_ATTRIBUTE(U)
#undef LLVM_DATA_SYMBOL_ATTRIBUTES_COMPARE_ATTRIBUTE
    return 0;
  }
  bool operator==(const SymbolAttributes &RHS) const {
    return compare(RHS) == 0;
  }
  bool operator!=(const SymbolAttributes &RHS) const {
    return compare(RHS) != 0;
  }
  bool operator<(const SymbolAttributes &RHS) const { return compare(RHS) < 0; }
  bool operator>(const SymbolAttributes &RHS) const { return compare(RHS) > 0; }
  bool operator<=(const SymbolAttributes &RHS) const {
    return compare(RHS) <= 0;
  }
  bool operator>=(const SymbolAttributes &RHS) const {
    return compare(RHS) >= 0;
  }

  void print(raw_ostream &OS) const;
  void dump() const;
  friend raw_ostream &operator<<(raw_ostream &OS, const SymbolAttributes &SA) {
    SA.print(OS);
    return OS;
  }

private:
  using SymbolAttributesEncodedT = uint16_t;

public:
  /// Guarantee the size of the attributes (for now?).
  static constexpr size_t EncodedSize = sizeof(SymbolAttributesEncodedT);
  void encode(SmallVectorImpl<char> &Data) const;
  static Expected<SymbolAttributes> consume(StringRef &Data);
  static Expected<SymbolAttributes> decode(StringRef Data);

  static constexpr SymbolAttributes getUndefined() {
    return SymbolAttributes(Scope::Undefined);
  }

  static constexpr SymbolAttributes getUndefinedOrNull() {
    return SymbolAttributes(Scope::UndefinedOrNull);
  }

  static constexpr SymbolAttributes
  getLocal(KeepAlive K = KeepAlive::Default,
           UnnamedAddress U = UnnamedAddress::Default) {
    return SymbolAttributes(Scope::Local, Hiding::Default, Linkage::Default, K,
                            U);
  }
  static constexpr SymbolAttributes getLocal(UnnamedAddress U) {
    return SymbolAttributes(Scope::Local, Hiding::Default, Linkage::Default,
                            KeepAlive::Default, U);
  }

  static SymbolAttributes get(const jitlink::Symbol &Symbol);

  explicit constexpr SymbolAttributes(
      Scope S = Scope::Local, Hiding H = Hiding::Default,
      Linkage L = Linkage::Default, KeepAlive K = KeepAlive::Default,
      UnnamedAddress U = UnnamedAddress::Default)
      : S(unsigned(S)), H(unsigned(H)), L(unsigned(L)), K(unsigned(K)),
        U(unsigned(U)) {
    assert(isValid() && "Expected valid symbol attributes");
  }
  explicit constexpr SymbolAttributes(
      Scope S, Linkage L, KeepAlive K = KeepAlive::Default,
      UnnamedAddress U = UnnamedAddress::Default)
      : SymbolAttributes(S, Hiding::Default, L, K, U) {}

private:
  constexpr bool isValid() const {
    // Most attributes are not applicable to undefined symbols.
    if (isUndefined())
      return getHiding() == Hiding::Default &&
             getLinkage() == Linkage::Default &&
             getKeepAlive() == KeepAlive::Default &&
             getUnnamedAddress() == UnnamedAddress::Default;

    // Local symbols have no linkage.
    if (isLocal())
      return getHiding() == Hiding::Default && getLinkage() == Linkage::Default;

    // Confirm the scope is valid.
    return isExported();
  }

  static constexpr NumScopeBits = 3;
  static constexpr NumHidingBits = 1;
  static constexpr NumLinkageBits = 1;
  static constexpr NumKeepAliveBits = 2;
  static constexpr NumUnnamedAddressBits = 2;

  unsigned S : NumScopeBits;
  unsigned H : NumHidingBits;
  unsigned L : NumLinkageBits;
  unsigned K : NumKeepAliveBits;
  unsigned U : NumUnnamedAddressBits;
};

/// The kind and offset of a fixup (e.g., for a relocation).
struct Fixup {
  jitlink::Edge::Kind Kind;
  jitlink::Edge::OffsetT Offset;

  bool operator==(const Fixup &RHS) const {
    return Kind == RHS.Kind && Offset == RHS.Offset;
  }
  bool operator!=(const Fixup &RHS) const { return !operator==(RHS); }
};

/// An encoded list of \a Fixup.
///
/// FIXME: Encode kinds separately from what jitlink has, since they're not
/// stable.
class FixupList {
public:
  class iterator
      : public iterator_facade_base<iterator, std::forward_iterator_tag,
                                    const Fixup> {
    friend class FixupList;

  public:
    const Fixup &operator*() const { return *F; }
    iterator &operator++() {
      decode();
      return *this;
    }
    using iterator::iterator_facade_base::operator++;

    bool operator==(const iterator &RHS) const {
      return F == RHS.F && Data.begin() == RHS.Data.begin() &&
             Data.end() == RHS.Data.end();
    }

  private:
    void decode(bool IsInit = false);

    struct EndTag {};
    iterator(EndTag, StringRef Data) : Data(Data.end(), 0) {}
    explicit iterator(StringRef Data) : Data(Data) { decode(/*IsInit=*/true); }

    StringRef Data;
    Optional<Fixup> F;
  };

  iterator begin() const { return iterator(Data); }
  iterator end() const { return iterator(iterator::EndTag{}, Data); }
  bool empty() const { return begin() == end(); }

  static void encode(ArrayRef<const jitlink::Edge *> Edges,
                     SmallVectorImpl<char> &Data);

  static void encode(ArrayRef<Fixup> Fixups, SmallVectorImpl<char> &Data);

  FixupList() = default;
  explicit FixupList(StringRef Data) : Data(Data) {}

private:
  StringRef Data;
};

/// Block data, including fixups but not targets. This embeds a \a FixupList
/// directly.
///
/// data   ::= header size content? fixup-list? alignment-offset?
/// header ::= 6-bit alignment | IsZeroFill | HasAlignmentOffset
class BlockData {
  enum : unsigned {
    NumAlignmentBits = 6,
    AlignmentMask = (1u << NumAlignmentBits) - 1u,
    HasAlignmentOffsetBit = NumAlignmentBits,
    IsZeroFillBit,
    AfterHeader = 1,
  };
  static_assert(IsZeroFillBit < 8, "Ran out of bits");

public:
  BlockData() = delete;
  explicit BlockData(StringRef Data) : Data(Data) {
    assert(Data.size() >= 2 && "Expected at least two bytes of data");
  }

  /// Encode block data.
  ///
  /// - Pass \a None for \p Content to make it zero-fill.
  /// - Pass \a None for \p Fixups to encode them externally.
  ///
  /// Uses minimum of 2B (1B + VBR8(Size)). \p AlignmentOffset, \p Content, and
  /// \p Fixups add no storage cost if they are not used.
  static void encode(uint64_t Size, uint64_t Alignment,
                     uint64_t AlignmentOffset, Optional<StringRef> Content,
                     ArrayRef<Fixup> Fixups, SmallVectorImpl<char> &Data);

  Error decode(uint64_t &Size, uint64_t &Alignment, uint64_t &AlignmentOffset,
               Optional<StringRef> &Content, FixupList &Fixups) const;

  bool isZeroFill() const { return front() & (1u << IsZeroFillBit); }
  uint64_t getSize() const {
    StringRef Remaining = Data.drop_front(AfterHeader);
    return consumeSizeFatal(Remaining);
  }
  uint64_t getAlignment() const {
    return llvm::decodeMaybeAlign(front() & AlignmentMask).valueOrOne().value();
  }
  uint64_t getAlignmentOffset() const {
    return hasAlignmentOffset() ? decodeAlignmentOffset() : 0;
  }
  Optional<ArrayRef<char>> getContentArray() const {
    if (Optional<StringRef> Content = getContent())
      return makeArrayRef(Content->begin(), Content->end());
    return None;
  }
  Optional<StringRef> getContent() const;
  FixupList getFixups() const;

private:
  static size_t getNumAlignmentOffsetBytes(uint64_t Alignment) {
    return Alignment < (1ULL << 8)
               ? 1
               : Alignment < (1ULL << 16) ? 2
                                          : Alignment < (1ULL << 32) ? 4 : 8;
  }
  static Error consumeContent(StringRef &Remaining, uint64_t Size,
                              Optional<StringRef> &Content);
  static Error consumeSize(StringRef &Remaining, uint64_t &Size);
  static uint64_t consumeSizeFatal(StringRef &Remaining);
  bool hasAlignmentOffset() const {
    return front() & (1u << HasAlignmentOffsetBit);
  }
  uint64_t decodeAlignmentOffset() const;
  uint8_t front() const { return Data.front(); }

  StringRef Data;
};

/// Information about how to apply a \a Fixup to a target, including the addend
/// and an index into the \a TargetList.
struct TargetInfo {
  /// Addend to apply to the target address.
  jitlink::Edge::AddendT Addend;

  /// Index into the list of targets.
  size_t Index;

  /// Print for debugging purposes.
  void print(raw_ostream &OS) const;
  void dump() const;
  friend raw_ostream &operator<<(raw_ostream &OS, const TargetInfo &TI) {
    TI.print(OS);
    return OS;
  }

  bool operator==(const TargetInfo &RHS) const {
    return Addend == RHS.Addend && Index == RHS.Index;
  }
  bool operator!=(const TargetInfo &RHS) const { return !operator==(RHS); }
};

/// An encoded list of \a TargetInfo, parallel with \a FixupList. This encodes
/// a target index and an addend.
class TargetInfoList {
public:
  class iterator
      : public iterator_facade_base<iterator, std::forward_iterator_tag,
                                    const TargetInfo> {
    friend class TargetInfoList;

  public:
    const TargetInfo &operator*() const { return *TI; }
    iterator &operator++() {
      decode();
      return *this;
    }
    using iterator::iterator_facade_base::operator++;

    bool operator==(const iterator &RHS) const {
      return TI == RHS.TI && Data.begin() == RHS.Data.begin() &&
             Data.end() == RHS.Data.end();
    }

  private:
    void decode(bool IsInit = false);

    struct EndTag {};
    iterator(EndTag, StringRef Data) : Data(Data.end(), 0) {}
    explicit iterator(StringRef Data) : Data(Data) { decode(/*IsInit=*/true); }

    StringRef Data;
    Optional<TargetInfo> TI;
  };

  iterator begin() const { return iterator(Data); }
  iterator end() const { return iterator(iterator::EndTag{}, Data); }

  static void encode(ArrayRef<TargetInfo> TIs, SmallVectorImpl<char> &Data);

  TargetInfoList() = default;
  explicit TargetInfoList(StringRef Data) : Data(Data) {}

private:
  StringRef Data;
};

} // end namespace data
} // end namespace casobjectformats
} // end namespace llvm

#endif // LLVM_CASOBJECTFORMATS_DATA_H
