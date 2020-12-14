//===- unittests/Support/OutputConfigTest.cpp - OutputConfig tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/OutputManager.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::vfs;

namespace {

TEST(PartialOutputConfigTest, construct) {
  EXPECT_TRUE(PartialOutputConfig().empty());
}

TEST(PartialOutputConfigTest, set) {
  constexpr ClientIntentOutputConfig ClientIntentValue =
      ClientIntentOutputConfig::NeedsSeeking;
  constexpr OnDiskOutputConfig OnDiskValue = OnDiskOutputConfig::UseTemporary;
  static_assert(unsigned(ClientIntentValue) == unsigned(OnDiskValue),
                "Coverage depends on having the same value representation");

  {
    auto ClientIntent = PartialOutputConfig().set(ClientIntentValue);
    EXPECT_FALSE(ClientIntent.empty());
    EXPECT_EQ(true, ClientIntent.test(ClientIntentValue));
    EXPECT_EQ(None, ClientIntent.test(OnDiskValue));
  }

  {
    auto OnDisk = PartialOutputConfig().set(OnDiskValue);
    EXPECT_FALSE(OnDisk.empty());
    EXPECT_EQ(true, OnDisk.test(OnDiskValue));
    EXPECT_EQ(None, OnDisk.test(ClientIntentValue));
  }

  {
    auto ClientIntentNotOnDisk =
        PartialOutputConfig().set(ClientIntentValue).set(OnDiskValue, false);
    EXPECT_EQ(true, ClientIntentNotOnDisk.test(ClientIntentValue));
    EXPECT_EQ(false, ClientIntentNotOnDisk.test(OnDiskValue));
  }

  {
    auto BothSetToTrue =
        PartialOutputConfig().set({ClientIntentValue, OnDiskValue});
    EXPECT_EQ(true, BothSetToTrue.test(ClientIntentValue));
    EXPECT_EQ(true, BothSetToTrue.test(OnDiskValue));
  }

  {
    auto BothSetToFalse =
        PartialOutputConfig().set({ClientIntentValue, OnDiskValue}, false);
    EXPECT_EQ(false, BothSetToFalse.test(ClientIntentValue));
    EXPECT_EQ(false, BothSetToFalse.test(OnDiskValue));
  }
}

TEST(PartialOutputConfigTest, reset) {
  {
    auto NotClientIntent =
        PartialOutputConfig().reset(ClientIntentOutputConfig::NeedsSeeking);
    EXPECT_FALSE(NotClientIntent.empty());
    EXPECT_EQ(false,
              NotClientIntent.test(ClientIntentOutputConfig::NeedsSeeking));
  }

  {
    auto BothDisabled =
        PartialOutputConfig().reset({ClientIntentOutputConfig::NeedsSeeking,
                                     OnDiskOutputConfig::UseTemporary});
    EXPECT_FALSE(BothDisabled.empty());
    EXPECT_EQ(false, BothDisabled.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(false, BothDisabled.test(ClientIntentOutputConfig::NeedsSeeking));
  }

  {
    auto ResetAfterSet = PartialOutputConfig()
                             .set(ClientIntentOutputConfig::NeedsSeeking)
                             .reset(ClientIntentOutputConfig::NeedsSeeking);
    EXPECT_EQ(false,
              ResetAfterSet.test(ClientIntentOutputConfig::NeedsSeeking));
  }

  {
    auto ResetAfterSetBoth = PartialOutputConfig()
                                 .set({ClientIntentOutputConfig::NeedsSeeking,
                                       OnDiskOutputConfig::UseTemporary})
                                 .reset({ClientIntentOutputConfig::NeedsSeeking,
                                         OnDiskOutputConfig::UseTemporary});
    EXPECT_EQ(false,
              ResetAfterSetBoth.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(false, ResetAfterSetBoth.test(OnDiskOutputConfig::UseTemporary));
  }

  {
    auto SetAfterReset = PartialOutputConfig()
                             .reset(ClientIntentOutputConfig::NeedsSeeking)
                             .set(ClientIntentOutputConfig::NeedsSeeking);
    EXPECT_EQ(true, SetAfterReset.test(ClientIntentOutputConfig::NeedsSeeking));
  }

  {
    auto SetAfterResetBoth = PartialOutputConfig()
                                 .reset({ClientIntentOutputConfig::NeedsSeeking,
                                         OnDiskOutputConfig::UseTemporary})
                                 .set({ClientIntentOutputConfig::NeedsSeeking,
                                       OnDiskOutputConfig::UseTemporary});
    EXPECT_EQ(true,
              SetAfterResetBoth.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, SetAfterResetBoth.test(OnDiskOutputConfig::UseTemporary));
  }
}

TEST(PartialOutputConfigTest, drop) {
  {
    auto DropClientIntent = PartialOutputConfig()
                                .set({ClientIntentOutputConfig::NeedsSeeking,
                                      OnDiskOutputConfig::UseTemporary})
                                .drop(ClientIntentOutputConfig::NeedsSeeking);
    EXPECT_FALSE(DropClientIntent.empty());
    EXPECT_EQ(None,
              DropClientIntent.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, DropClientIntent.test(OnDiskOutputConfig::UseTemporary));
  }

  {
    auto DropClientIntentAfterReset =
        PartialOutputConfig()
            .reset({ClientIntentOutputConfig::NeedsSeeking,
                    OnDiskOutputConfig::UseTemporary})
            .drop(ClientIntentOutputConfig::NeedsSeeking);
    EXPECT_FALSE(DropClientIntentAfterReset.empty());
    EXPECT_EQ(None, DropClientIntentAfterReset.test(
                        ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(false, DropClientIntentAfterReset.test(
                         OnDiskOutputConfig::UseTemporary));
  }

  {
    auto DropOnDisk = PartialOutputConfig()
                          .set({ClientIntentOutputConfig::NeedsSeeking,
                                OnDiskOutputConfig::UseTemporary})
                          .drop(OnDiskOutputConfig::UseTemporary);
    EXPECT_FALSE(DropOnDisk.empty());
    EXPECT_EQ(true, DropOnDisk.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(None, DropOnDisk.test(OnDiskOutputConfig::UseTemporary));
  }

  {
    auto DropBoth = PartialOutputConfig()
                        .set({ClientIntentOutputConfig::NeedsSeeking,
                              OnDiskOutputConfig::UseTemporary})
                        .drop({OnDiskOutputConfig::UseTemporary,
                               ClientIntentOutputConfig::NeedsSeeking});
    EXPECT_TRUE(DropBoth.empty());
    EXPECT_EQ(None, DropBoth.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(None, DropBoth.test(OnDiskOutputConfig::UseTemporary));
  }

  {
    auto DropBothAfterReset =
        PartialOutputConfig()
            .reset({ClientIntentOutputConfig::NeedsSeeking,
                    OnDiskOutputConfig::UseTemporary})
            .drop({OnDiskOutputConfig::UseTemporary,
                   ClientIntentOutputConfig::NeedsSeeking});
    EXPECT_TRUE(DropBothAfterReset.empty());
    EXPECT_EQ(None,
              DropBothAfterReset.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(None, DropBothAfterReset.test(OnDiskOutputConfig::UseTemporary));
  }
}

TEST(PartialOutputConfigTest, applyOverrides) {
  auto getConfig = []() {
    PartialOutputConfig Config;
    Config.set(ClientIntentOutputConfig::NeedsSeeking);
    Config.set(OnDiskOutputConfig::UseTemporary);
    Config.reset(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories);
    Config.reset(OnDiskOutputConfig::RemoveFileOnSignal);
    return Config;
  };

  {
    // No overrides.
    PartialOutputConfig Config = getConfig(), Overrides;
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // No config, just overrides.
    PartialOutputConfig Overrides = getConfig(), Config;
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // Some the same, some different.
    PartialOutputConfig Config = getConfig(), Overrides;
    Overrides.set(ClientIntentOutputConfig::NeedsSeeking);
    Overrides.reset(OnDiskOutputConfig::UseTemporary);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // New settings.
    PartialOutputConfig Config = getConfig(), Overrides;
    Overrides.set(ClientIntentOutputConfig::NeedsReadAccess);
    Overrides.reset(OnDiskOutputConfig::OpenFlagText);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsReadAccess));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::OpenFlagText));
  }
}

TEST(PartialOutputConfigTest, applyDefaults) {
  auto getConfig = []() {
    PartialOutputConfig Config;
    Config.set(ClientIntentOutputConfig::NeedsSeeking);
    Config.set(OnDiskOutputConfig::UseTemporary);
    Config.reset(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories);
    Config.reset(OnDiskOutputConfig::RemoveFileOnSignal);
    return Config;
  };

  {
    // No defaults.
    PartialOutputConfig Config = getConfig(), Defaults;
    Config.applyDefaults(Defaults);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // No config, just defaults.
    PartialOutputConfig Defaults = getConfig(), Config;
    Config.applyDefaults(Defaults);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // Some the same, some different.
    PartialOutputConfig Config = getConfig(), Defaults;
    Defaults.set(ClientIntentOutputConfig::NeedsSeeking);
    Defaults.reset(OnDiskOutputConfig::UseTemporary);
    Config.applyDefaults(Defaults);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // New settings.
    PartialOutputConfig Config = getConfig(), Overrides;
    Overrides.set(ClientIntentOutputConfig::NeedsReadAccess);
    Overrides.reset(OnDiskOutputConfig::OpenFlagText);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsReadAccess));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::OpenFlagText));
  }
}

TEST(OutputConfigTest, construct) {
  EXPECT_TRUE(OutputConfig().none());
  EXPECT_FALSE(OutputConfig().test(ClientIntentOutputConfig::NeedsSeeking));
  EXPECT_FALSE(OutputConfig().test(OnDiskOutputConfig::UseTemporary));

  {
    OutputConfig Config = {ClientIntentOutputConfig::NeedsSeeking};
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_TRUE(Config.test(ClientIntentOutputConfig::NeedsSeeking));
  }

  {
    OutputConfig Config = {ClientIntentOutputConfig::NeedsSeeking,
                           OnDiskOutputConfig::UseTemporary};
    EXPECT_FALSE(Config.none());
    EXPECT_TRUE(Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_TRUE(Config.test(ClientIntentOutputConfig::NeedsSeeking));
  }
}

TEST(OutputConfigTest, set) {
  {
    // Set one.
    OutputConfig Config;
    Config.set(ClientIntentOutputConfig::NeedsSeeking);
    EXPECT_FALSE(Config.none());
    EXPECT_TRUE(Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_FALSE(Config.test(OnDiskOutputConfig::UseTemporary));

    // Undo it.
    Config.set(ClientIntentOutputConfig::NeedsSeeking, false);
    EXPECT_TRUE(Config.none());
    EXPECT_FALSE(Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_FALSE(Config.test(OnDiskOutputConfig::UseTemporary));
  }

  {
    // Set multiple.
    OutputConfig Config;
    Config.set({ClientIntentOutputConfig::NeedsSeeking,
                OnDiskOutputConfig::UseTemporary,
                OnDiskOutputConfig::UseTemporaryCreateMissingDirectories});
    EXPECT_FALSE(Config.none());
    EXPECT_TRUE(Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_TRUE(Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_TRUE(
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));

    // Undo two of them.
    Config.set({ClientIntentOutputConfig::NeedsSeeking,
                OnDiskOutputConfig::UseTemporaryCreateMissingDirectories},
               false);
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_FALSE(
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_TRUE(Config.test(OnDiskOutputConfig::UseTemporary));
  }
}

TEST(OutputConfigTest, reset) {
  constexpr ClientIntentOutputConfig ClientIntentValue =
      ClientIntentOutputConfig::NeedsSeeking;
  constexpr OnDiskOutputConfig OnDiskValue = OnDiskOutputConfig::UseTemporary;
  static_assert(unsigned(ClientIntentValue) == unsigned(OnDiskValue),
                "Coverage depends on having the same value representation");

  {
    // Reset one.
    OutputConfig Config = {ClientIntentValue, OnDiskValue};
    Config.reset(ClientIntentValue);
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(ClientIntentValue));
    EXPECT_TRUE(Config.test(OnDiskValue));
  }

  {
    // Reset both.
    OutputConfig Config = {ClientIntentValue, OnDiskValue};
    Config.reset({ClientIntentValue, OnDiskValue});
    EXPECT_TRUE(Config.none());
    EXPECT_FALSE(Config.test(ClientIntentValue));
    EXPECT_FALSE(Config.test(OnDiskValue));
  }

  {
    // Reset multiple (but not all).
    OutputConfig Config = {
        ClientIntentValue, OnDiskValue,
        OnDiskOutputConfig::UseTemporaryCreateMissingDirectories};
    Config.reset({ClientIntentValue,
                  OnDiskOutputConfig::UseTemporaryCreateMissingDirectories});
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(ClientIntentValue));
    EXPECT_FALSE(
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_TRUE(Config.test(OnDiskValue));
  }
}

TEST(OutputConfigTest, applyOverrides) {
  OutputConfig InitialConfig = {
      ClientIntentOutputConfig::NeedsSeeking,
      OnDiskOutputConfig::UseTemporary,
  };

  {
    // No overrides.
    OutputConfig Config = InitialConfig;
    PartialOutputConfig Overrides;
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
  }

  {
    // Some the same, some different.
    OutputConfig Config = InitialConfig;
    PartialOutputConfig Overrides;
    Overrides.set(ClientIntentOutputConfig::NeedsSeeking);
    Overrides.reset(OnDiskOutputConfig::UseTemporary);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::OpenFlagText));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
  }

  {
    // New settings.
    OutputConfig Config = InitialConfig;
    PartialOutputConfig Overrides;
    Overrides.set(ClientIntentOutputConfig::NeedsReadAccess);
    Overrides.reset(OnDiskOutputConfig::OpenFlagText);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsSeeking));
    EXPECT_EQ(true, Config.test(ClientIntentOutputConfig::NeedsReadAccess));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(
        false,
        Config.test(OnDiskOutputConfig::UseTemporaryCreateMissingDirectories));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::OpenFlagText));
  }
}

} // anonymous namespace
