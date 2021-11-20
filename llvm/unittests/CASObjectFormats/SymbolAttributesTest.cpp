//===- SymbolAttributesTest.cpp - Unit tests for SymbolAttributes ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CASObjectFormats/Data.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::casobjectformats;
using namespace llvm::casobjectformats::data;

namespace {

TEST(SymbolAttributesTest, construct) {
  // Confirm accessors work after construction, despite any bit-packing.
  EXPECT_EQ(Scope::Local, SymbolAttributes().getScope());
  EXPECT_EQ(Hiding::Default, SymbolAttributes().getHiding());
  EXPECT_EQ(Linkage::Default, SymbolAttributes().getLinkage());
  EXPECT_EQ(KeepAlive::Default, SymbolAttributes().getKeepAlive());
  EXPECT_EQ(UnnamedAddress::Default, SymbolAttributes().getUnnamedAddress());

  SymbolAttributes Global(Scope::Global);
  SymbolAttributes Hideable(Scope::Global, Hiding::Hideable);
  SymbolAttributes Weak(Scope::Global, Linkage::Weak);
  SymbolAttributes Used = SymbolAttributes::getLocal(KeepAlive::Always);
  SymbolAttributes Unnamed = SymbolAttributes::getLocal(UnnamedAddress::Global);
  SymbolAttributes Undefined = SymbolAttributes::getUndefined();
  SymbolAttributes UndefinedOrNull = SymbolAttributes::getUndefinedOrNull();

  EXPECT_EQ(Scope::Undefined, Undefined.getScope());
  EXPECT_EQ(Scope::UndefinedOrNull, UndefinedOrNull.getScope());
  for (auto SA : {Global, Hideable, Weak})
    EXPECT_EQ(Scope::Global, SA.getScope());
  for (auto SA : {Used, Unnamed})
    EXPECT_EQ(Scope::Local, SA.getScope());

  EXPECT_EQ(Hiding::Hideable, Hideable.getHiding());
  for (auto SA : {Global, Weak, Used, Unnamed, Undefined, UndefinedOrNull})
    EXPECT_EQ(Hiding::Default, SA.getHiding());

  EXPECT_EQ(Linkage::Weak, Weak.getLinkage());
  for (auto SA : {Global, Hideable, Used, Unnamed, Undefined, UndefinedOrNull})
    EXPECT_EQ(Linkage::Default, SA.getLinkage());

  EXPECT_EQ(KeepAlive::Always, Used.getKeepAlive());
  for (auto SA : {Global, Hideable, Weak, Unnamed, Undefined, UndefinedOrNull})
    EXPECT_EQ(KeepAlive::Default, SA.getKeepAlive());

  EXPECT_EQ(UnnamedAddress::Global, Unnamed.getUnnamedAddress());
  for (auto SA : {Global, Hideable, Weak, Used, Undefined, UndefinedOrNull})
    EXPECT_EQ(UnnamedAddress::Default, SA.getUnnamedAddress());
}

TEST(SymbolAttributesTest, print) {
  auto toString = [](const auto &V) {
    std::string S;
    raw_string_ostream(S) << V;
    return S;
  };

  // Check printing enum values.
  EXPECT_EQ("local", toString(Scope::Local));
  EXPECT_EQ("global", toString(Scope::Global));
  EXPECT_EQ("protected", toString(Scope::Protected));
  EXPECT_EQ("hidden", toString(Scope::Hidden));
  EXPECT_EQ("undef", toString(Scope::Undefined));
  EXPECT_EQ("undef_or_null", toString(Scope::UndefinedOrNull));

  EXPECT_EQ("default", toString(Linkage::Default));
  EXPECT_EQ("weak", toString(Linkage::Weak));

  EXPECT_EQ("default", toString(Hiding::Default));
  EXPECT_EQ("hideable", toString(Hiding::Hideable));

  EXPECT_EQ("none", toString(KeepAlive::Default));
  EXPECT_EQ("referenced", toString(KeepAlive::Referenced));
  EXPECT_EQ("always", toString(KeepAlive::Always));

  EXPECT_EQ("default", toString(UnnamedAddress::Default));
  EXPECT_EQ("local", toString(UnnamedAddress::Local));
  EXPECT_EQ("global", toString(UnnamedAddress::Global));

  // Spot check printing combined attributes.
  EXPECT_EQ("undef", toString(SymbolAttributes::getUndefined()));
  EXPECT_EQ("global", toString(SymbolAttributes(Scope::Global)));
  EXPECT_EQ("local", toString(SymbolAttributes::getLocal()));
  EXPECT_EQ(
      "hidden+hideable+weak+keep=referenced+noaddr=local",
      toString(SymbolAttributes(Scope::Hidden, Hiding::Hideable, Linkage::Weak,
                                KeepAlive::Referenced, UnnamedAddress::Local)));
}

TEST(SymbolAttributesTest, equals) {
  // Spot check operator==.
  SymbolAttributes PlainGlobal(Scope::Global);
  EXPECT_TRUE(PlainGlobal == PlainGlobal);
  EXPECT_FALSE(PlainGlobal != PlainGlobal);

  // List attributes different in exactly one axis.
  SymbolAttributes Others[] = {
      SymbolAttributes(Scope::Local),
      SymbolAttributes(Scope::Global, Hiding::Hideable),
      SymbolAttributes(Scope::Global, Linkage::Weak),
      SymbolAttributes(Scope::Global, Linkage::Default, KeepAlive::Always),
      SymbolAttributes(Scope::Global, Linkage::Default, KeepAlive::Default,
                       UnnamedAddress::Global),
  };
  for (SymbolAttributes Other : Others) {
    EXPECT_EQ(Other, Other);
    EXPECT_NE(PlainGlobal, Other);
  }
}

TEST(SymbolAttributesTest, setters) {
  {
    SymbolAttributes Global(Scope::Global);

    // Spot check setters.
    Global.setHiding(Hiding::Hideable);
    EXPECT_EQ(Hiding::Hideable, Global.getHiding());

    Global.setLinkage(Linkage::Weak);
    EXPECT_EQ(Linkage::Weak, Global.getLinkage());

    Global.setUnnamedAddress(UnnamedAddress::Global);
    EXPECT_EQ(UnnamedAddress::Global, Global.getUnnamedAddress());

    Global.setKeepAlive(KeepAlive::Always);
    EXPECT_EQ(KeepAlive::Always, Global.getKeepAlive());
  }

  // Check assertions for undefined.
  for (SymbolAttributes Undef : {SymbolAttributes::getUndefined(),
                                 SymbolAttributes::getUndefinedOrNull()}) {
    // Okay if it's a no-op.
    Undef.setHiding(Hiding::Default);
    Undef.setLinkage(Linkage::Default);
    Undef.setUnnamedAddress(UnnamedAddress::Default);
    Undef.setKeepAlive(KeepAlive::Default);

    // Assert otherwise.
#if !defined(NDEBUG) && GTEST_HAS_DEATH_TEST
    EXPECT_DEATH(Undef.setHiding(Hiding::Hideable), "Only exported symbols");
    EXPECT_DEATH(Undef.setLinkage(Linkage::Weak), "Only exported symbols");
    EXPECT_DEATH(Undef.setUnnamedAddress(UnnamedAddress::Global),
                 "undefined symbol");
    EXPECT_DEATH(Undef.setKeepAlive(KeepAlive::Always), "undefined symbol");
#endif
  }

  // Check assertions for local.
  {
    SymbolAttributes Local(Scope::Local);

    // Okay for no-ops.
    Local.setHiding(Hiding::Default);
    Local.setLinkage(Linkage::Default);

    // Assert otherwise.
#if !defined(NDEBUG) && GTEST_HAS_DEATH_TEST
    EXPECT_DEATH(Local.setHiding(Hiding::Hideable), "Only exported symbols");
    EXPECT_DEATH(Local.setLinkage(Linkage::Weak), "Only exported symbols");
#endif

    // Other attributes should work.
    Local.setUnnamedAddress(UnnamedAddress::Global);
    EXPECT_EQ(UnnamedAddress::Global, Local.getUnnamedAddress());

    Local.setKeepAlive(KeepAlive::Always);
    EXPECT_EQ(KeepAlive::Always, Local.getKeepAlive());
  }
}

#if !defined(NDEBUG) && GTEST_HAS_DEATH_TEST
TEST(SymbolAttributesTest, constructInvalid) {
  // Check for an assertion on each possible invalid value, but don't bother
  // checking all combinations of those.
  std::string AssertionMessage = "Expected valid symbol attributes";

  // Check undefined symbols don't have other attributes.
  for (Scope S : {Scope::Undefined, Scope::UndefinedOrNull}) {
    EXPECT_DEATH(SymbolAttributes(S, Hiding::Hideable), AssertionMessage);
    EXPECT_DEATH(SymbolAttributes(S, Linkage::Weak), AssertionMessage);
    EXPECT_DEATH(SymbolAttributes(S, Linkage::Default, KeepAlive::Referenced),
                 AssertionMessage);
    EXPECT_DEATH(SymbolAttributes(S, Linkage::Default, KeepAlive::Always),
                 AssertionMessage);
    EXPECT_DEATH(SymbolAttributes(S, Linkage::Default, KeepAlive::Default,
                                  UnnamedAddress::Local),
                 AssertionMessage);
    EXPECT_DEATH(SymbolAttributes(S, Linkage::Default, KeepAlive::Default,
                                  UnnamedAddress::Global),
                 AssertionMessage);
  }

  // Check local symbols don't have linkage-related attributes.
  EXPECT_DEATH(SymbolAttributes(Scope::Local, Hiding::Hideable),
               AssertionMessage);
  EXPECT_DEATH(SymbolAttributes(Scope::Local, Linkage::Weak), AssertionMessage);
}
#endif

static void fillWithValid(SmallVectorImpl<SymbolAttributes> &Attrs) {
  // Total number of (valid) combinations smaller than 256.
  Attrs.reserve(256);

  Attrs.push_back(SymbolAttributes::getUndefined());
  Attrs.push_back(SymbolAttributes::getUndefinedOrNull());

  for (KeepAlive K :
       {KeepAlive::Default, KeepAlive::Referenced, KeepAlive::Always}) {
    for (UnnamedAddress U : {UnnamedAddress::Default, UnnamedAddress::Local,
                             UnnamedAddress::Global}) {
      Attrs.push_back(SymbolAttributes::getLocal(K, U));

      for (Scope S : {Scope::Hidden, Scope::Global, Scope::Protected}) {
        for (Hiding H : {Hiding::Default, Hiding::Hideable}) {
          for (Linkage L : {Linkage::Default, Linkage::Weak}) {
            Attrs.push_back(SymbolAttributes(S, H, L, K, U));
          }
        }
      }
    }
  }
}

TEST(SymbolAttributesTest, properties) {
  SmallVector<SymbolAttributes, 256> All;
  fillWithValid(All);

  for (SymbolAttributes SA : All) {
    // Should be mutually exclusive.
    EXPECT_EQ(1U, 0U + SA.isUndefined() + SA.isExported() + SA.isLocal());

    // Check isUsed().
    EXPECT_EQ(SA.getKeepAlive() == KeepAlive::Always, SA.isUsed());

    // Check isAutoHide().
    if (SA.isAutoHide()) {
      EXPECT_FALSE(SA.isUndefined());
      EXPECT_FALSE(SA.isLocal());
      EXPECT_NE(UnnamedAddress::Default, SA.getUnnamedAddress());
      EXPECT_EQ(Hiding::Hideable, SA.getHiding());
      EXPECT_EQ(Linkage::Weak, SA.getLinkage());
    }

    // Check isHid*AfterLink().
    EXPECT_EQ(SA.getScope() == Scope::Local || SA.getScope() == Scope::Hidden,
              SA.isHiddenAfterLink());
    if (SA.isHiddenAfterLink()) {
      EXPECT_TRUE(SA.isHideableAfterLink());
    } else {
      EXPECT_EQ(SA.getHiding() == Hiding::Hideable, SA.isHideableAfterLink());
    }

    // Check canDiscard*().
    if (SA.isUsed()) {
      EXPECT_FALSE(SA.canDiscard());
      EXPECT_FALSE(SA.canDiscardAfterLink());
    } else if (!SA.isExported()) {
      EXPECT_TRUE(SA.canDiscard());
      EXPECT_TRUE(SA.canDiscardAfterLink());
    } else {
      EXPECT_EQ(SA.getKeepAlive() == KeepAlive::Referenced, SA.canDiscard());
      if (SA.canDiscard()) {
        EXPECT_TRUE(SA.canDiscardAfterLink());
      } else {
        EXPECT_EQ(SA.isHideableAfterLink(), SA.canDiscardAfterLink());
      }
    }

    // Check canMergeConstants().
    EXPECT_EQ(SA.getUnnamedAddress() != UnnamedAddress::Default,
              SA.canMergeConstants());
  }
}

TEST(SymbolAttributesTest, encode) {
  SmallVector<SymbolAttributes, 256> All;
  fillWithValid(All);

  for (SymbolAttributes SA : All) {
    Optional<SymbolAttributes> RoundTrip;

    SmallString<16> Data;
    SA.encode(Data);

    // Compare even when decode() returns an Error. The input value will be
    // printed in the assertion that follows to help with debugging.
    EXPECT_THAT_ERROR(SymbolAttributes::decode(Data).moveInto(RoundTrip),
                      Succeeded());
    ASSERT_EQ(SA, RoundTrip);
  }
}

TEST(SymbolAttributesTest, encodeAfter) {
  for (StringRef Initial : {"", "arbitrary string"}) {
    SymbolAttributes Local = SymbolAttributes::getLocal();
    SymbolAttributes Global(Scope::Global);

    SmallString<64> Storage = Initial;
    Local.encode(Storage);
    Global.encode(Storage);
    ASSERT_EQ(Initial.size() + 2u, Storage.size());

    StringRef Data = Storage;
    SymbolAttributes RoundTripLocal;
    SymbolAttributes RoundTripGlobal;
    EXPECT_THAT_ERROR(SymbolAttributes::decode(Data.take_back(2).drop_back())
                          .moveInto(RoundTripLocal),
                      Succeeded());
    EXPECT_THAT_ERROR(
        SymbolAttributes::decode(Data.take_back()).moveInto(RoundTripGlobal),
        Succeeded());
    EXPECT_EQ(Local, RoundTripLocal);
    EXPECT_EQ(Global, RoundTripGlobal);

    EXPECT_THAT_ERROR(SymbolAttributes::decode(Data.take_back(2)).takeError(),
                      Failed());
  }
}

TEST(SymbolAttributesTest, decodeEmptyString) {
  EXPECT_THAT_ERROR(SymbolAttributes::decode(StringRef()).takeError(),
                    Failed());
}

TEST(SymbolAttributesTest, consume) {
  for (StringRef Suffix : {"", "arbitrary string"}) {
    SymbolAttributes Local = SymbolAttributes::getLocal();
    SymbolAttributes Global(Scope::Global);

    SmallString<64> Storage;
    Local.encode(Storage);
    Global.encode(Storage);
    ASSERT_EQ(2u, Storage.size());
    Storage.append(Suffix);

    StringRef Data = Storage;
    SymbolAttributes RoundTripLocal;
    SymbolAttributes RoundTripGlobal;
    EXPECT_THAT_ERROR(SymbolAttributes::consume(Data).moveInto(RoundTripLocal),
                      Succeeded());
    EXPECT_THAT_ERROR(SymbolAttributes::consume(Data).moveInto(RoundTripGlobal),
                      Succeeded());
    EXPECT_EQ(Local, RoundTripLocal);
    EXPECT_EQ(Global, RoundTripGlobal);
    EXPECT_EQ(Suffix, Data);
  }
  StringRef Empty;
  EXPECT_THAT_ERROR(SymbolAttributes::consume(Empty).takeError(), Failed());
}

} // end namespace
