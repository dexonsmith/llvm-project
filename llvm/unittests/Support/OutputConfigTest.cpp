//===- OutputConfigTest.cpp - OutputConfig gets --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/OutputConfig.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::vfs;

namespace {

TEST(OutputConfigTest, construct) {
  EXPECT_FALSE(OutputConfig().getText());
  EXPECT_FALSE(OutputConfig().getTextWithCRLF());
  EXPECT_FALSE(OutputConfig().getVolatile());
  EXPECT_TRUE(OutputConfig().getCrashCleanup());
  EXPECT_TRUE(OutputConfig().getAtomicWrite());
  EXPECT_TRUE(OutputConfig().getImplyCreateDirectories());
  EXPECT_TRUE(OutputConfig().getOverwrite());

  {
    OutputConfig Config = {OutputConfigFlag::NoCrashCleanup};
    EXPECT_FALSE(Config.getText());
    EXPECT_FALSE(Config.getTextWithCRLF());
    EXPECT_FALSE(Config.getVolatile());
    EXPECT_FALSE(Config.getCrashCleanup());
    EXPECT_TRUE(Config.getAtomicWrite());
    EXPECT_TRUE(Config.getImplyCreateDirectories());
    EXPECT_TRUE(Config.getOverwrite());
  }

  {
    OutputConfig Config = {OutputConfigFlag::Text,
                           OutputConfigFlag::NoCrashCleanup};
    EXPECT_TRUE(Config.getText());
    EXPECT_FALSE(Config.getTextWithCRLF());
    EXPECT_FALSE(Config.getVolatile());
    EXPECT_FALSE(Config.getCrashCleanup());
    EXPECT_TRUE(Config.getAtomicWrite());
    EXPECT_TRUE(Config.getImplyCreateDirectories());
    EXPECT_TRUE(Config.getOverwrite());
  }
}

TEST(OutputConfigTest, set) {
  {
    // Set one flag to true and set its negation.
    OutputConfig Config;
    Config.set(OutputConfigFlag::Text);
    EXPECT_TRUE(Config.get(OutputConfigFlag::Text));
    Config.set(OutputConfigFlag::NoText);
    EXPECT_FALSE(Config.get(OutputConfigFlag::Text));

    // Set flags to false.
    Config.set(OutputConfigFlag::NoText, false);
    EXPECT_TRUE(Config.get(OutputConfigFlag::Text));
    Config.set(OutputConfigFlag::Text, false);
    EXPECT_FALSE(Config.get(OutputConfigFlag::Text));
  }

  {
    // Set multiple flags.
    OutputConfig Config;
    Config.set({OutputConfigFlag::Text, OutputConfigFlag::NoCrashCleanup,
                OutputConfigFlag::NoImplyCreateDirectories});
    EXPECT_TRUE(Config.get(OutputConfigFlag::Text));
    EXPECT_TRUE(Config.get(OutputConfigFlag::NoCrashCleanup));
    EXPECT_TRUE(Config.get(OutputConfigFlag::NoImplyCreateDirectories));

    // Undo two of them.
    Config.set(
        {OutputConfigFlag::Text, OutputConfigFlag::NoImplyCreateDirectories},
        false);
    EXPECT_FALSE(Config.get(OutputConfigFlag::Text));
    EXPECT_FALSE(Config.get(OutputConfigFlag::NoImplyCreateDirectories));
    EXPECT_TRUE(Config.get(OutputConfigFlag::NoCrashCleanup));
  }
}

TEST(OutputConfigTest, printFlag) {
  auto toString = [](OutputConfigFlag Flag) {
    std::string Printed;
    raw_string_ostream OS(Printed);
    OutputConfig::printFlag(OS, Flag);
    return Printed;
  };
  EXPECT_EQ("Text", toString(OutputConfigFlag::Text));
  EXPECT_EQ("NoText", toString(OutputConfigFlag::NoText));
  EXPECT_EQ("CrashCleanup", toString(OutputConfigFlag::CrashCleanup));
  EXPECT_EQ("NoCrashCleanup", toString(OutputConfigFlag::NoCrashCleanup));
}

TEST(OutputConfigTest, print) {
  auto toString = [](OutputConfig Config) {
    std::string Printed;
    raw_string_ostream OS(Printed);
    Config.print(OS);
    return Printed;
  };
  EXPECT_EQ("{}", toString(OutputConfig()));
  EXPECT_EQ("{Text}", toString({OutputConfigFlag::Text}));
  EXPECT_EQ(
      "{Text,NoCrashCleanup}",
      toString({OutputConfigFlag::Text, OutputConfigFlag::NoCrashCleanup}));
  EXPECT_EQ("{Text,NoCrashCleanup}", toString({OutputConfigFlag::NoCrashCleanup,
                                               OutputConfigFlag::Text}));
}

} // anonymous namespace
