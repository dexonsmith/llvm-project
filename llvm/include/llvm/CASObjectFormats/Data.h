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

/// Flags for a symbol.
enum class SymbolFlags : unsigned short {
  /// No flags.
  None = 0U,

  /// Callable.
  ///
  /// LLVM IR: function, ifuncs, and aliases of functions and ifuncs.
  Callable = 1U << 0,

  /// Defined. (Otherwise, undefined / external / declaration only.)
  Defined = 1U << 1,

  /// Exported. May be referenced by other compile units (and barring other
  /// flags, other linkage units).
  ///
  /// Requires: !Undefined.
  ///
  /// LLVM IR: not "internal" or "private".
  Exported = Defined | 1U << 2,

  /// Weak linkage: one definition is selected at link time.
  ///
  /// LLVM IR: "common", "weak", "weak_odr", "linkonce", and "linkonce_odr".
  Weak = Exported | 1U << 3,

  /// Exported from the compile unit, but not the linkage unit. Can be
  /// referenced from other compile units.
  ///
  /// The link effectively downgrades to local by not including this in the
  /// dynamic symbol table.
  ///
  /// LLVM IR: "hidden" visibility.
  Hidden = Exported | 1U << 4,

  /// Exported, but cannot be overridden. Only relevant for ELF.
  ///
  /// LLVM IR: "protected" visibility.
  Protected = Exported | 1U << 5,

  /// Bits for visibility. Mutually exclusive.
  VisibilityFlags = (Hidden | Protected) & ~Exported,

  /// Discardable when not referenced despite being exported. In practice,
  /// cannot be dead-stripped if \a ImplicitlyUsed.
  ///
  /// LLVM IR: "linkonce" and "linkonce_odr". "available_externally" also
  /// matches this, although LLVM always discards it before lowering.
  Discardable = Exported | 1U << 6,
  Linkonce = Weak | Discardable,

  /// "One definition rule". Language guarantee that other exported symbols
  /// with the same name are semantically equivalent.
  ///
  /// LLVM IR: "weak_odr" and "linkonce_odr".
  ODR = Exported | 1U << 7,
  WeakODR = Weak | ODR,
  LinkonceODR = Linkonce | ODR,

  /// Bits that depend on \a Exported.
  ExportedFlags = (Exported | Weak | Hidden | ODR | Discardable) & ~Defined,

  /// Address is not taken for comparison locally. If exported, it's possible
  /// that another compile unit or linkage unit takes its address and compares
  /// it. Otherwise, can be upgraded to \a GlobalUnnamedAddress.
  ///
  /// During linking, if symbol resolution joins two symbols the the resulting
  /// symbol only has this property if all symbols have it. In that case, if
  /// the symbol is no longer exported, it can be upgraded to \a
  /// GlobalUnnamedAddress.
  ///
  /// LLVM IR: "local_unnamed_addr" and "unnamed_addr".
  UnnamedAddress = Defined | 1U << 8,

  /// Address known not to be taken. Typically, if !Exported and UnnamedAddress
  /// and constant, then GlobalUnnamedAddress.
  ///
  /// If !Exported, this can be deduplicated / CSE'd with any symbol
  /// that has equivalent content.
  /// LLVM IR: "unnamed_addr".
  GlobalUnnamedAddress = UnnamedAddress | 1U << 9,

  /// Bits that depend on \a UnnamedAddress.
  UnnamedAddressFlags = UnnamedAddress | GlobalUnnamedAddress,

  /// Dependencies for \a Autohide.
  AutohideDependencies = LinkonceODR | UnnamedAddress,

  /// Can be removed from the symbol table (no longer Exported) without
  /// changing program semantics, even if it cannot be discarded due to
  /// existing references.
  ///
  /// LLVM IR: "linkonce_odr" with "unnamed_addr", or when constant (such as
  /// function definitions) with "local_unnamed_addr".
  ///
  /// FIXME: Can we find a way to safely remove this flag? It's mostly implied
  /// by others.
  Autohide = AutohideDependencies | 1U << 10,
  AutohideFlags = Autohide & ~AutohideDependencies,

  /// Undefined / externally defined, or possibly null.
  ///
  /// Requires: !Defined.
  ///
  /// LLVM IR: "extern_weak" linkage.
  ExternWeak = 1U << 11,

  /// Bits that require !Defined. No other bits are valid if one of these is
  /// set.
  UndefinedFlags = ExternWeak,

  /// As-if there's another reference that cannot be seen. This cannot be
  /// dead-stripped.
  ///
  /// TODO: Where applicable, use inverted liveness edges instead. See \a
  /// HasInvertedLivenessEdge below.
  ///
  /// LLVM IR: member of "!llvm.used".
  ImplicitlyUsed = Defined | 1U << 12,

  /// Flag to indicate that the definition has an outgoing edge that has
  /// "inverted" liveness semantics. Such an edge means that instead of the
  /// source (this symbol) keeping the target alive, the target keeps alive the
  /// source, and if the target is discarded then this should be too.
  ///
  /// This allows metadata content (such an FDR in EH frames, or a slice of
  /// debug info specific to a function) to be kept alive by the thing it
  /// describes *without* modifying it.
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
  HasInvertedLivenessEdge = Defined | 1U << 13,

  /// Flag to indicate this is a template in the object format encoding. This
  /// helps to improve deduplication of EH frame symbols in the nestedv1
  /// format where symbols currently reference their transitive call graph, by
  /// using a placeholder edge to the function that gets fixed up later using
  /// the function's keep-alive edge to the EH frame.
  ///
  /// FIXME: Stop relying on this and remove it. Note that flatv1 uses this for
  /// all blocks/symbols.
  EncodingTemplate = Defined | 1U << 14,

  /// Bits that are only valid if Defined and !Exported.
  LocalFlags = (HasInvertedLivenessEdge | EncodingTemplate) & ~Defined,

  /// Bits that are only valid if Defined.
  DefinedFlags = Defined | ExportedFlags | LocalFlags | UnnamedAddressFlags | ImplicitlyUsed,

  /// Bits that must not be set if Exported.
  IllegalFlagsIfDefined = UndefinedFlags,

  /// Bits that must not be set if Undefined. That's almost everything.
  IllegalFlagsIfUndefined = DefinedFlags,

  /// Bits that must not be set if Exported.
  IllegalFlagsIfExported = IllegalFlagsIfDefined | LocalFlags,

  /// Bits that must not be set if Local.
  IllegalFlagsIfLocal = IllegalFlagsIfDefined | ExportedFlags,

  LLVM_MARK_AS_BITMASK_ENUM(/*LargestValue=*/EncodingTemplate)
};
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// Container for \a SymbolFlags that has convenient accessors and knows how to
/// encode itself.
class SymbolAttributes {
  bool test(SymbolFlags TestFlags) const { return (Flags & TestFlags) != SymbolFlags::None; }

public:
  SymbolFlags getFlags() const { return Flags; }

  bool isCallable() const { return test(SymbolFlags::Callable); }

  bool isDefined() const { return test(SymbolFlags::Defined); }
  bool isLocal() const { return !isDefined() && !isExported(); }

  bool isUndefined() const { return !isDefined(); }
  bool isExternWeak() const { return test(SymbolFlags::ExternWeak); }
  bool isExternStrong() const { return isUndefined() && !isExternWeak(); }

  bool isExported() const { return test(SymbolFlags::Exported); }
  bool isWeak() const { return test(SymbolFlags::Weak); }
  bool isHidden() const { return test(SymbolFlags::Hidden); }
  bool isProtected() const { return test(SymbolFlags::Protected); }
  bool isDiscardable() const { return test(SymbolFlags::Discardable); }
  bool isLinkonce() const { return test(SymbolFlags::Linkonce); }
  bool isWeakODR() const { return test(SymbolFlags::WeakODR); }
  bool isLinkonceODR() const { return test(SymbolFlags::LinkonceODR); }

  bool isUnnamedAddress() const { return test(SymbolFlags::UnnamedAddress); }
  bool isGlobalUnnamedAddress() const {
    return test(SymbolFlags::GlobalUnnamedAddress);
  }
  bool isLocalUnnamedAddress() const {
    return isUnnamedAddress() && !isGlobalUnnamedAddress();
  }

  bool isAutohide() const { return test(SymbolFlags::Autohide); }

  bool isImplicitlyUsed() const { return test(SymbolFlags::ImplicitlyUsed); }

  bool hasInvertedLivenessEdge() const {
    return test(SymbolFlags::HasInvertedLivenessEdge);
  }

  bool isEncodingTemplate() const {
    return test(SymbolFlags::EncodingTemplate);
  }

  /// Set the new flags, dropping most no-longer-valid conflicting flags.
  void set(SymbolFlags NewFlags) {
    if ((NewFlags & SymbolFlags::Exported) != SymbolFlags::None)
      Flags &= ~SymbolFlags::IllegalFlagsIfExported;
    else if ((NewFlags & SymbolFlags::Defined) != SymbolFlags::None)
      Flags &= ~SymbolFlags::IllegalFlagsIfLocal;
    else
      Flags &= ~SymbolFlags::IllegalFlagsIfUndefined;

    if (NewFlags & SymbolFlags::VisibilityFlags)
      Flags &= ~SymbolFlags::VisibilityFlags;

    Flags |= NewFlags;
    assert(isValid() && "Expected valid flag combination");
  }

  /// Reset flags, also dropping no-longer-valid derivative flags.
  void reset(SymbolFlags FlagsToDrop) {
    if ((FlagsToDrop & SymbolFlags::Defined) != SymbolFlags::None)
      Flags &= ~SymbolFlags::DefinedFlags;
    if ((FlagsToDrop & SymbolFlags::Exported) != SymbolFlags::None)
      Flags &= ~SymbolFlags::ExportedFlags;
    if ((FlagsToDrop & SymbolFlags::UnnamedAddress) != SymbolFlags::None)
      Flags &= ~SymbolFlags::UnnamedAddressFlags;
    if ((FlagsToDrop & SymbolFlags::Autohide) != SymbolFlags::None)
      Flags &= ~SymbolFlags::AutohideFlags;

    Flags &= ~FlagsToDrop;
    assert(isValid() && "Expected valid flag combination");
  }

  /// Set to a local symbol, dropping any flags that require Exported or
  /// !Defined, and adding \p NewFlags.
  void setLocal(SymbolFlags NewFlags = SymbolFlags::Defined) {
    assert(!(NewFlags & SymbolFlags::Exported) && "Expected local symbol");

    reset(SymbolFlags::IllegalFlagsIfLocal);
    set(NewFlags | SymbolFlags::Defined);
  }

  /// Set to an exported symbol, dropping any flags that require !Exported or
  /// !Defined, and adding \p NewFlags.
  void setExported(SymbolFlags NewFlags = SymbolFlags::Exported) {
    reset(SymbolFlags::IllegalFlagsIfExported);
    set(NewFlags | SymbolFlags::Exported);
  }

  /// Set to an exported symbol with \p Visibility. Passing \a
  /// SymbolFlags::Default drops \a SymbolFlags::Hidden or \a
  /// SymbolFlags::Protected, if set.
  void setExportedVisibility(SymbolFlags Visibility) {
    assert((Visibility == SymbolFlags::Default ||
            Visibility == SymbolFlags::Hidden ||
            Visibility == SymbolFlags::Protected) &&
           "Expected default, hidden, or protected visibility");
    setExported(Visibility);
  }

  void setUndefined() { reset(SymbolFlags::IllegalFlagsIfUndefined); }
  void setExternWeak() {
    setUndefined();
    set(SymbolFlags::ExternWeak);
  }
  void setExternStrong() {
    setUndefined();
    reset(SymbolFlags::ExternWeak);
  }

  int compare(const SymbolAttributes &RHS) const {
    return Flags == RHS.Flags ? 0 : Flags < RHS.Flags ? -1 : 1;
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

  void encode(SmallVectorImpl<char> &Data) const;
  static Expected<SymbolAttributes> decode(StringRef Data);
  static Expected<SymbolAttributes> consume(StringRef &Data);

  static SymbolAttributes get(const jitlink::Symbol &Symbol);

  explicit SymbolAttributes(SymbolFlags Flags) : Flags(Flags) {
    assert(isValid() && "Expected valid symbol attributes");
  }

private:
  bool isValid() const;

  SymbolFlags Flags = SymbolFlags::Default;
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
