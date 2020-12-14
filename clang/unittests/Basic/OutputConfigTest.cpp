//===- unittests/Basic/OutputConfigTest.cpp - OutputConfig tests ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/OutputManager.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace llvm;

namespace {

TEST(PartialOutputConfigTest, construct) {
  EXPECT_TRUE(PartialOutputConfig().empty());
  EXPECT_EQ(None, PartialOutputConfig().test(InMemoryOutputConfig::Enabled));
  EXPECT_EQ(None, PartialOutputConfig().test(OnDiskOutputConfig::Enabled));
}

TEST(PartialOutputConfigTest, set) {
  {
    auto InMemory = PartialOutputConfig().set(InMemoryOutputConfig::Enabled);
    EXPECT_FALSE(InMemory.empty());
    EXPECT_EQ(true, InMemory.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(None, InMemory.test(OnDiskOutputConfig::Enabled));
  }

  {
    auto OnDisk = PartialOutputConfig().set(OnDiskOutputConfig::Enabled);
    EXPECT_FALSE(OnDisk.empty());
    EXPECT_EQ(true, OnDisk.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(None, OnDisk.test(InMemoryOutputConfig::Enabled));
  }

  {
    auto InMemoryNotOnDisk = PartialOutputConfig()
                                 .set(InMemoryOutputConfig::Enabled)
                                 .set(OnDiskOutputConfig::Enabled, false);
    EXPECT_EQ(true, InMemoryNotOnDisk.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(false, InMemoryNotOnDisk.test(OnDiskOutputConfig::Enabled));
  }

  {
    auto BothEnabled = PartialOutputConfig().set(
        {InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled});
    EXPECT_EQ(true, BothEnabled.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, BothEnabled.test(OnDiskOutputConfig::Enabled));
  }

  {
    auto BothDisabled = PartialOutputConfig().set(
        {InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled}, false);
    EXPECT_EQ(false, BothDisabled.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(false, BothDisabled.test(OnDiskOutputConfig::Enabled));
  }
}

TEST(PartialOutputConfigTest, reset) {
  {
    auto NotInMemory =
        PartialOutputConfig().reset(InMemoryOutputConfig::Enabled);
    EXPECT_FALSE(NotInMemory.empty());
    EXPECT_EQ(false, NotInMemory.test(InMemoryOutputConfig::Enabled));
  }

  {
    auto BothDisabled = PartialOutputConfig().reset(
        {InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled});
    EXPECT_FALSE(BothDisabled.empty());
    EXPECT_EQ(false, BothDisabled.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(false, BothDisabled.test(InMemoryOutputConfig::Enabled));
  }

  {
    auto ResetAfterSet = PartialOutputConfig()
                             .set(InMemoryOutputConfig::Enabled)
                             .reset(InMemoryOutputConfig::Enabled);
    EXPECT_EQ(false, ResetAfterSet.test(InMemoryOutputConfig::Enabled));
  }

  {
    auto ResetAfterSetBoth =
        PartialOutputConfig()
            .set({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled})
            .reset(
                {InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled});
    EXPECT_EQ(false, ResetAfterSetBoth.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(false, ResetAfterSetBoth.test(OnDiskOutputConfig::Enabled));
  }

  {
    auto SetAfterReset = PartialOutputConfig()
                             .reset(InMemoryOutputConfig::Enabled)
                             .set(InMemoryOutputConfig::Enabled);
    EXPECT_EQ(true, SetAfterReset.test(InMemoryOutputConfig::Enabled));
  }

  {
    auto SetAfterResetBoth =
        PartialOutputConfig()
            .reset({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled})
            .set({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled});
    EXPECT_EQ(true, SetAfterResetBoth.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, SetAfterResetBoth.test(OnDiskOutputConfig::Enabled));
  }
}

TEST(PartialOutputConfigTest, drop) {
  {
    auto DropInMemory =
        PartialOutputConfig()
            .set({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled})
            .drop(InMemoryOutputConfig::Enabled);
    EXPECT_FALSE(DropInMemory.empty());
    EXPECT_EQ(None, DropInMemory.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, DropInMemory.test(OnDiskOutputConfig::Enabled));
  }

  {
    auto DropInMemoryAfterReset =
        PartialOutputConfig()
            .reset({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled})
            .drop(InMemoryOutputConfig::Enabled);
    EXPECT_FALSE(DropInMemoryAfterReset.empty());
    EXPECT_EQ(None, DropInMemoryAfterReset.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(false, DropInMemoryAfterReset.test(OnDiskOutputConfig::Enabled));
  }

  {
    auto DropOnDisk =
        PartialOutputConfig()
            .set({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled})
            .drop(OnDiskOutputConfig::Enabled);
    EXPECT_FALSE(DropOnDisk.empty());
    EXPECT_EQ(true, DropOnDisk.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(None, DropOnDisk.test(OnDiskOutputConfig::Enabled));
  }

  {
    auto DropBoth =
        PartialOutputConfig()
            .set({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled})
            .drop({OnDiskOutputConfig::Enabled, InMemoryOutputConfig::Enabled});
    EXPECT_TRUE(DropBoth.empty());
    EXPECT_EQ(None, DropBoth.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(None, DropBoth.test(OnDiskOutputConfig::Enabled));
  }

  {
    auto DropBothAfterReset =
        PartialOutputConfig()
            .reset({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled})
            .drop({OnDiskOutputConfig::Enabled, InMemoryOutputConfig::Enabled});
    EXPECT_TRUE(DropBothAfterReset.empty());
    EXPECT_EQ(None, DropBothAfterReset.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(None, DropBothAfterReset.test(OnDiskOutputConfig::Enabled));
  }
}

TEST(PartialOutputConfigTest, applyOverrides) {
  auto getConfig = []() {
    PartialOutputConfig Config;
    Config.set(InMemoryOutputConfig::Enabled);
    Config.set(OnDiskOutputConfig::Enabled);
    Config.reset(OnDiskOutputConfig::UseTemporary);
    Config.reset(OnDiskOutputConfig::RemoveFileOnSignal);
    return Config;
  };

  {
    // No overrides.
    PartialOutputConfig Config = getConfig(), Overrides;
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // No config, just overrides.
    PartialOutputConfig Overrides = getConfig(), Config;
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // Some the same, some different.
    PartialOutputConfig Config = getConfig(), Overrides;
    Overrides.set(InMemoryOutputConfig::Enabled);
    Overrides.reset(OnDiskOutputConfig::Enabled);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // New settings.
    PartialOutputConfig Config = getConfig(), Overrides;
    Overrides.set(InMemoryOutputConfig::ReuseWriteThroughBuffer);
    Overrides.reset(OnDiskOutputConfig::OpenFlagText);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::ReuseWriteThroughBuffer));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::OpenFlagText));
  }
}

TEST(PartialOutputConfigTest, applyDefaults) {
  auto getConfig = []() {
    PartialOutputConfig Config;
    Config.set(InMemoryOutputConfig::Enabled);
    Config.set(OnDiskOutputConfig::Enabled);
    Config.reset(OnDiskOutputConfig::UseTemporary);
    Config.reset(OnDiskOutputConfig::RemoveFileOnSignal);
    return Config;
  };

  {
    // No defaults.
    PartialOutputConfig Config = getConfig(), Defaults;
    Config.applyDefaults(Defaults);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // No config, just defaults.
    PartialOutputConfig Defaults = getConfig(), Config;
    Config.applyDefaults(Defaults);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // Some the same, some different.
    PartialOutputConfig Config = getConfig(), Defaults;
    Defaults.set(InMemoryOutputConfig::Enabled);
    Defaults.reset(OnDiskOutputConfig::Enabled);
    Config.applyDefaults(Defaults);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(None, Config.test(OnDiskOutputConfig::OpenFlagText));
  }

  {
    // New settings.
    PartialOutputConfig Config = getConfig(), Overrides;
    Overrides.set(InMemoryOutputConfig::ReuseWriteThroughBuffer);
    Overrides.reset(OnDiskOutputConfig::OpenFlagText);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::ReuseWriteThroughBuffer));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::OpenFlagText));
  }
}

TEST(OutputConfigTest, construct) {
  EXPECT_TRUE(OutputConfig().none());
  EXPECT_FALSE(OutputConfig().test(InMemoryOutputConfig::Enabled));
  EXPECT_FALSE(OutputConfig().test(OnDiskOutputConfig::Enabled));

  {
    OutputConfig InMemoryEnabled = {InMemoryOutputConfig::Enabled};
    EXPECT_FALSE(InMemoryEnabled.none());
    EXPECT_FALSE(InMemoryEnabled.test(OnDiskOutputConfig::Enabled));
    EXPECT_TRUE(InMemoryEnabled.test(InMemoryOutputConfig::Enabled));
  }

  {
    OutputConfig BothEnabled = {InMemoryOutputConfig::Enabled,
                                OnDiskOutputConfig::Enabled};
    EXPECT_FALSE(BothEnabled.none());
    EXPECT_TRUE(BothEnabled.test(OnDiskOutputConfig::Enabled));
    EXPECT_TRUE(BothEnabled.test(InMemoryOutputConfig::Enabled));
  }
}

TEST(OutputConfigTest, set) {
  {
    // Set one.
    OutputConfig Config;
    Config.set(InMemoryOutputConfig::Enabled);
    EXPECT_FALSE(Config.none());
    EXPECT_TRUE(Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_FALSE(Config.test(OnDiskOutputConfig::Enabled));

    // Undo it.
    Config.set(InMemoryOutputConfig::Enabled, false);
    EXPECT_TRUE(Config.none());
    EXPECT_FALSE(Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_FALSE(Config.test(OnDiskOutputConfig::Enabled));
  }

  {
    // Set multiple.
    OutputConfig Config;
    Config.set({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled,
                OnDiskOutputConfig::UseTemporary});
    EXPECT_FALSE(Config.none());
    EXPECT_TRUE(Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_TRUE(Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_TRUE(Config.test(OnDiskOutputConfig::UseTemporary));

    // Undo two of them.
    Config.set(
        {InMemoryOutputConfig::Enabled, OnDiskOutputConfig::UseTemporary},
        false);
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_FALSE(Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_TRUE(Config.test(OnDiskOutputConfig::Enabled));
  }
}

TEST(OutputConfigTest, reset) {
  {
    // Reset one.
    OutputConfig Config = {InMemoryOutputConfig::Enabled,
                           OnDiskOutputConfig::Enabled};
    Config.reset(InMemoryOutputConfig::Enabled);
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_TRUE(Config.test(OnDiskOutputConfig::Enabled));
  }

  {
    // Reset both.
    OutputConfig Config = {InMemoryOutputConfig::Enabled,
                           OnDiskOutputConfig::Enabled};
    Config.reset({InMemoryOutputConfig::Enabled, OnDiskOutputConfig::Enabled});
    EXPECT_TRUE(Config.none());
    EXPECT_FALSE(Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_FALSE(Config.test(OnDiskOutputConfig::Enabled));
  }

  {
    // Reset multiple (but not all).
    OutputConfig Config = {InMemoryOutputConfig::Enabled,
                           OnDiskOutputConfig::Enabled,
                           OnDiskOutputConfig::UseTemporary};
    Config.reset(
        {InMemoryOutputConfig::Enabled, OnDiskOutputConfig::UseTemporary});
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_FALSE(Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_TRUE(Config.test(OnDiskOutputConfig::Enabled));
  }
}

TEST(OutputConfigTest, applyOverrides) {
  OutputConfig InitialConfig = {
      InMemoryOutputConfig::Enabled,
      OnDiskOutputConfig::Enabled,
  };

  {
    // No overrides.
    OutputConfig Config = InitialConfig;
    PartialOutputConfig Overrides;
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
  }

  {
    // Some the same, some different.
    OutputConfig Config = InitialConfig;
    PartialOutputConfig Overrides;
    Overrides.set(InMemoryOutputConfig::Enabled);
    Overrides.reset(OnDiskOutputConfig::Enabled);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::OpenFlagText));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
  }

  {
    // New settings.
    OutputConfig Config = InitialConfig;
    PartialOutputConfig Overrides;
    Overrides.set(InMemoryOutputConfig::ReuseWriteThroughBuffer);
    Overrides.reset(OnDiskOutputConfig::OpenFlagText);
    Config.applyOverrides(Overrides);
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::Enabled));
    EXPECT_EQ(true, Config.test(InMemoryOutputConfig::ReuseWriteThroughBuffer));
    EXPECT_EQ(true, Config.test(OnDiskOutputConfig::Enabled));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::UseTemporary));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::RemoveFileOnSignal));
    EXPECT_EQ(false, Config.test(OnDiskOutputConfig::OpenFlagText));
  }
}

} // anonymous namespace
