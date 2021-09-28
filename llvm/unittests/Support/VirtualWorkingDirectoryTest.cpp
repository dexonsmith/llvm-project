//===- VirtualWorkingDirectoryTest.cpp - Virtual working directory tests --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/VirtualWorkingDirectory.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::vfs;

using Style = sys::path::Style;
using WorkingDirectoryState = sys::path::WorkingDirectoryState;

namespace {

TEST(VirtualWorkingDirectoryTest, construct) {
  {
    VirtualWorkingDirectory VWD;
    EXPECT_EQ(VWD.getSettings(),
              VirtualWorkingDirectory::Settings::getVirtual());
    EXPECT_EQ(VirtualWorkingDirectory::CWD_Raw,
              VWD.getSettings().CanonicalizeOnSet);
    EXPECT_FALSE(VWD.hasVirtualState());
  }

  for (Style PathStyle : {Style::native, Style::posix, Style::windows}) {
    WorkingDirectoryState WDS(PathStyle);
    WDS.setCurrentWorkingDirectory("//root/path");

    VirtualWorkingDirectory VWD(std::move(WDS));
    EXPECT_EQ(VWD.getSettings(),
              VirtualWorkingDirectory::Settings::getVirtual());
    EXPECT_EQ(VirtualWorkingDirectory::CWD_Raw,
              VWD.getSettings().CanonicalizeOnSet);
    EXPECT_TRUE(VWD.hasVirtualState());
    EXPECT_EQ(PathStyle, VWD.getVirtualState().getPathStyle());
    EXPECT_FALSE(WDS.hasCurrentWorkingDirectory());
    ASSERT_TRUE(VWD.getVirtualState().hasCurrentWorkingDirectory());
    EXPECT_EQ("//root/path",
              *VWD.getVirtualState().getCurrentWorkingDirectory());
  }
}

TEST(VirtualWorkingDirectoryTest, makeAbsolute) {
  StringRef CWD = "/root/nested/path";
  StringRef Filename = "filename";
  StringRef FilenameDot = "./filename";
  StringRef FilenameDotDot = "../filename";
  StringRef Combined = "/root/nested/path/filename";
  StringRef CombinedDot = "/root/nested/path/./filename";
  StringRef CombinedDotDot = "/root/nested/path/../filename";

  VirtualWorkingDirectory VWD;
  SmallString<128> Path = Filename;
  EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Failed());
  EXPECT_EQ(Filename, Path);

  VWD.initializeVirtualState(Style::posix);
  EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Failed());
  EXPECT_EQ(Filename, Path);

  // Check makeAbsolute, and confirm it doesn't canonicalize.
  EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory(CWD), Succeeded());
  for (auto Canonicalize : {
           VirtualWorkingDirectory::CWD_Raw,
           VirtualWorkingDirectory::CWD_KeepDotDot,
           VirtualWorkingDirectory::CWD_RemoveDotDot,
       }) {
    VWD.getSettings().CanonicalizeOnSet = Canonicalize;

    Path = Filename;
    EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Succeeded());
    EXPECT_EQ(Combined, Path);

    Path = FilenameDot;
    EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Succeeded());
    EXPECT_EQ(CombinedDot, Path);

    Path = FilenameDotDot;
    EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Succeeded());
    EXPECT_EQ(CombinedDotDot, Path);
  }
}

TEST(VirtualWorkingDirectoryTest, setCurrentWorkingDirectory) {
  {
    VirtualWorkingDirectory VWD;
    VWD.getSettings() = VirtualWorkingDirectory::Settings::getError();

    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory("//some/root"), Failed());
    VWD.initializeVirtualState();
    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory("//some/root"),
                      Succeeded());

    SmallString<128> Path;
    EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Succeeded());
    EXPECT_EQ("//some/root/", Path);
  }

  {
    VirtualWorkingDirectory VWD;
    VWD.getSettings() = VirtualWorkingDirectory::Settings::getVirtual();

    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory("//some/root"),
                      Succeeded());

    SmallString<128> Path;
    EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Succeeded());
    EXPECT_EQ("//some/root/", Path);
  }

  {
    VirtualWorkingDirectory VWD;
    VWD.getSettings() = VirtualWorkingDirectory::Settings::getInitLive();

    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory("."), Succeeded());

    SmallString<128> Path;
    EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Succeeded());
    SmallString<128> LivePath;
    EXPECT_FALSE(sys::fs::current_path(LivePath));
    EXPECT_EQ((LivePath + "/").str(), Path);
  }
}

TEST(VirtualWorkingDirectoryTest, setCurrentWorkingDirectoryRelative) {
  StringRef CWD = "/root/nested/path";
  StringRef Subdir = "subdir";
  StringRef SubdirDot = "./subdir";
  StringRef SubdirDotDot = "../subdir";
  StringRef Filename = "filename";
  StringRef Combined = "/root/nested/path/subdir/filename";
  StringRef CombinedDot = "/root/nested/path/./subdir/filename";
  StringRef CombinedDotDot = "/root/nested/path/../subdir/filename";
  StringRef CombinedBack = "/root/nested/subdir/filename";

  VirtualWorkingDirectory VWD;
  VWD.initializeVirtualState(Style::posix);
  EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory(CWD), Succeeded());
  EXPECT_EQ(CWD, VWD.getVirtualState().getCurrentWorkingDirectory());
  for (auto Canonicalize : {
           VirtualWorkingDirectory::CWD_Raw,
           VirtualWorkingDirectory::CWD_KeepDotDot,
           VirtualWorkingDirectory::CWD_RemoveDotDot,
       }) {
    VWD.getSettings().CanonicalizeOnSet = Canonicalize;

    SmallString<128> Path = Filename;
    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory(CWD), Succeeded());
    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory(Subdir), Succeeded());
    EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Succeeded());
    EXPECT_EQ(Combined, Path);

    Path = Filename;
    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory(CWD), Succeeded());
    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory(SubdirDot), Succeeded());
    EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Succeeded());
    if (Canonicalize == VirtualWorkingDirectory::CWD_Raw)
      EXPECT_EQ(CombinedDot, Path);
    else
      EXPECT_EQ(Combined, Path);

    Path = Filename;
    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory(CWD), Succeeded());
    EXPECT_THAT_ERROR(VWD.setCurrentWorkingDirectory(SubdirDotDot),
                      Succeeded());
    EXPECT_THAT_ERROR(VWD.makeAbsolute(Path), Succeeded());
    if (Canonicalize == VirtualWorkingDirectory::CWD_RemoveDotDot)
      EXPECT_EQ(CombinedBack, Path);
    else
      EXPECT_EQ(CombinedDotDot, Path);
  }
}

} // end namespace
