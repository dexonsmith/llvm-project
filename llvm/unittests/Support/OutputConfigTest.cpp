//===- unittests/Support/OutputConfigTest.cpp - OutputConfig tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/OutputBackend.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::vfs;

namespace {

TEST(OutputConfigTest, construct) {
  EXPECT_TRUE(OutputConfig().none());
  EXPECT_FALSE(OutputConfig().test(OutputConfigFlag::Text));
  EXPECT_FALSE(OutputConfig().test(OutputConfigFlag::NoCrashCleanup));

  {
    OutputConfig Config = {OutputConfigFlag::NoCrashCleanup};
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(OutputConfigFlag::Text));
    EXPECT_TRUE(Config.test(OutputConfigFlag::NoCrashCleanup));
  }

  {
    OutputConfig Config = {OutputConfigFlag::Text,
                           OutputConfigFlag::NoCrashCleanup};
    EXPECT_FALSE(Config.none());
    EXPECT_TRUE(Config.test(OutputConfigFlag::Text));
    EXPECT_TRUE(Config.test(OutputConfigFlag::NoCrashCleanup));
  }
}

TEST(OutputConfigTest, set) {
  {
    // Set one.
    OutputConfig Config;
    Config.set(OutputConfigFlag::Text);
    EXPECT_FALSE(Config.none());
    EXPECT_TRUE(Config.test(OutputConfigFlag::Text));
    EXPECT_FALSE(Config.test(OutputConfigFlag::NoCrashCleanup));

    // Undo it.
    Config.set(OutputConfigFlag::Text, false);
    EXPECT_TRUE(Config.none());
    EXPECT_FALSE(Config.test(OutputConfigFlag::Text));
    EXPECT_FALSE(Config.test(OutputConfigFlag::NoCrashCleanup));
  }

  {
    // Set multiple.
    OutputConfig Config;
    Config.set({OutputConfigFlag::Text, OutputConfigFlag::NoCrashCleanup,
                OutputConfigFlag::NoImplyCreateDirectories});
    EXPECT_FALSE(Config.none());
    EXPECT_TRUE(Config.test(OutputConfigFlag::Text));
    EXPECT_TRUE(Config.test(OutputConfigFlag::NoCrashCleanup));
    EXPECT_TRUE(Config.test(OutputConfigFlag::NoImplyCreateDirectories));

    // Undo two of them.
    Config.set(
        {OutputConfigFlag::Text, OutputConfigFlag::NoImplyCreateDirectories},
        false);
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(OutputConfigFlag::Text));
    EXPECT_FALSE(Config.test(OutputConfigFlag::NoImplyCreateDirectories));
    EXPECT_TRUE(Config.test(OutputConfigFlag::NoCrashCleanup));
  }
}

TEST(OutputConfigTest, reset) {
  {
    // Reset one.
    OutputConfig Config = {OutputConfigFlag::Text,
                           OutputConfigFlag::NoCrashCleanup};
    Config.reset(OutputConfigFlag::Text);
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(OutputConfigFlag::Text));
    EXPECT_TRUE(Config.test(OutputConfigFlag::NoCrashCleanup));
  }

  {
    // Reset both.
    OutputConfig Config = {OutputConfigFlag::Text,
                           OutputConfigFlag::NoCrashCleanup};
    Config.reset({OutputConfigFlag::Text, OutputConfigFlag::NoCrashCleanup});
    EXPECT_TRUE(Config.none());
    EXPECT_FALSE(Config.test(OutputConfigFlag::Text));
    EXPECT_FALSE(Config.test(OutputConfigFlag::NoCrashCleanup));
  }

  {
    // Reset multiple (but not all).
    OutputConfig Config = {OutputConfigFlag::Text,
                           OutputConfigFlag::NoCrashCleanup,
                           OutputConfigFlag::NoImplyCreateDirectories};
    Config.reset(
        {OutputConfigFlag::Text, OutputConfigFlag::NoImplyCreateDirectories});
    EXPECT_FALSE(Config.none());
    EXPECT_FALSE(Config.test(OutputConfigFlag::Text));
    EXPECT_FALSE(Config.test(OutputConfigFlag::NoImplyCreateDirectories));
    EXPECT_TRUE(Config.test(OutputConfigFlag::NoCrashCleanup));
  }
}

} // anonymous namespace
