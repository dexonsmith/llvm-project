//===- WorkingDirectoryStateTest.cpp - Working directory state tests ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/WorkingDirectoryState.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::sys;
using namespace llvm::sys::path;

namespace {

TEST(WorkingDirectoryStateTest, construct) {
  WorkingDirectoryState WD;
  EXPECT_TRUE(WD.isEmpty());
  EXPECT_EQ(Style::native, WD.getPathStyle());
  EXPECT_EQ(Style::native, WorkingDirectoryState(Style::native).getPathStyle());
  EXPECT_EQ(system_style(), WD.getEffectivePathStyle());

  EXPECT_FALSE(WD.getCurrentWorkingDirectory());
}

TEST(WorkingDirectoryStateTest, constructPosix) {
  WorkingDirectoryState WD(Style::posix);
  EXPECT_TRUE(WD.isEmpty());
  EXPECT_FALSE(WD.getCurrentWorkingDirectory());

  EXPECT_EQ(Style::posix, WD.getPathStyle());
  EXPECT_EQ(Style::posix, WD.getEffectivePathStyle());
}

TEST(WorkingDirectoryStateTest, constructWindows) {
  WorkingDirectoryState WD(Style::windows);
  EXPECT_TRUE(WD.isEmpty());
  EXPECT_FALSE(WD.getCurrentWorkingDirectory());

  EXPECT_EQ(Style::windows, WD.getPathStyle());
  EXPECT_EQ(Style::windows, WD.getEffectivePathStyle());
}

TEST(WorkingDirectoryStateTest, getEffectivePathStyle) {
  EXPECT_EQ(system_style(),
            WorkingDirectoryState::getEffectivePathStyle(Style::native));
  EXPECT_EQ(Style::posix,
            WorkingDirectoryState::getEffectivePathStyle(Style::posix));
  EXPECT_EQ(Style::windows,
            WorkingDirectoryState::getEffectivePathStyle(Style::windows));
}

TEST(WorkingDirectoryStateTest, isValidDrive) {
  // Check posix.
  EXPECT_FALSE(WorkingDirectoryState::isValidDrive('A', Style::posix));
  EXPECT_FALSE(WorkingDirectoryState::isValidDrive('L', Style::posix));
  EXPECT_FALSE(WorkingDirectoryState::isValidDrive('Z', Style::posix));

  // Check windows.
  EXPECT_TRUE(WorkingDirectoryState::isValidDrive('A', Style::windows));
  EXPECT_TRUE(WorkingDirectoryState::isValidDrive('L', Style::windows));
  EXPECT_TRUE(WorkingDirectoryState::isValidDrive('Z', Style::windows));

  // Confirm native matches the system style.
  EXPECT_EQ(WorkingDirectoryState::isValidDrive('A', system_style()),
            WorkingDirectoryState::isValidDrive('A', Style::native));
  EXPECT_EQ(WorkingDirectoryState::isValidDrive('Z', system_style()),
            WorkingDirectoryState::isValidDrive('Z', Style::native));

  // Check some invalid drives.
  for (Style PathStyle : {Style::native, Style::posix, Style::windows}) {
    for (char Drive : {char('A' - 1), char('Z' + 1), 'a', 'z', '0', char(0)}) {
      EXPECT_FALSE(WorkingDirectoryState::isValidDrive(Drive, PathStyle));
    }
  }

  // Check the member function.
  for (Style PathStyle : {Style::native, Style::posix, Style::windows}) {
    for (char Drive : {'A', 'Z', '0'}) {
      EXPECT_EQ(WorkingDirectoryState::isValidDrive(Drive, PathStyle),
                WorkingDirectoryState(PathStyle).isValidDrive(Drive));
    }
  }
}

TEST(WorkingDirectoryStateTest, isAbsolute) {
  // Check posix.
  for (StringRef P : {
           "/",
           "//",
           "//a",
           "/./",
           "/../",
           "/\\",
           "/a",
           "/./a",
           "/../a",
       }) {
    EXPECT_TRUE(WorkingDirectoryState::isAbsolute(P, Style::posix));
    EXPECT_FALSE(WorkingDirectoryState::isAbsolute(P, Style::windows));
  }

  // Check Windows drives.
  for (StringRef P : {
           "A:/ab",
           "a:/ab",
           "A:\\ab",
           "a:\\ab",
           "A:/",
           "a:/",
           "A:\\",
           "a:\\",
           "Z:/",
           "z:/",
           "Z:\\",
           "z:\\",
       }) {
    EXPECT_TRUE(WorkingDirectoryState::isAbsolute(P, Style::windows));
    EXPECT_FALSE(WorkingDirectoryState::isAbsolute(P, Style::posix));
  }

  // Check UNC paths, including partial roots.
  for (StringRef P : {
           "//./.",
           "//a/b",
           "\\\\.\\.",
           "\\\\a\\b",
       }) {
    EXPECT_TRUE(WorkingDirectoryState::isAbsolute(P, Style::windows));
    EXPECT_EQ(P[0] == '/', WorkingDirectoryState::isAbsolute(P, Style::posix));
  }

  // Check paths that are never absolute.
  for (StringRef P : {
           "",
           "./",
           "../",
           "\\",
           "\\\\a",
           "\\\\",
           "a",
           "./a",
           "../a",
           "A:",
           "a:",
           "A:a",
           "A:.",
           "A:..",
           "A:a\\",
           "A:.\\",
           "A:..\\",
       }) {
    EXPECT_FALSE(WorkingDirectoryState::isAbsolute(P, Style::windows));
    EXPECT_FALSE(WorkingDirectoryState::isAbsolute(P, Style::posix));
  }

  // Confirm native matches the system style.
  EXPECT_EQ(WorkingDirectoryState::isAbsolute("/", system_style()),
            WorkingDirectoryState::isAbsolute("/", Style::native));
  EXPECT_EQ(WorkingDirectoryState::isAbsolute("A:\\", system_style()),
            WorkingDirectoryState::isAbsolute("A:\\", Style::native));

  // Check the member function.
  for (Style PathStyle : {Style::native, Style::posix, Style::windows}) {
    for (StringRef P : {
             "/",
             "//"
             "/./",
             "/../",
             "/\\",
             "/a",
             "/./a",
             "/../a",
             "A:/ab",
             "a:/ab",
             "A:\\ab",
             "a:\\ab",
             "A:/",
             "a:/",
             "A:\\",
             "a:\\",
             "Z:/",
             "z:/",
             "Z:\\",
             "z:\\",
             "//a",
             "//a/b",
             "//./.",
             "\\\\a",
             "\\\\a\\b",
             "\\\\.\\.",
             "",
             "./",
             "../",
             "\\",
             "\\\\",
             "a",
             "./a",
             "../a",
             "A:",
             "a:a",
             "A:a",
             "A:.",
             "A:..",
             "A:a\\",
             "A:.\\",
             "A:..\\",
         }) {
      EXPECT_EQ(WorkingDirectoryState::isAbsolute(P, PathStyle),
                WorkingDirectoryState(PathStyle).isAbsolute(P));
    }
  }
}

TEST(WorkingDirectoryStateTest, getDriveForPath) {
  // Check posix.
  EXPECT_EQ(None, WorkingDirectoryState::getDriveForPath("A:", Style::posix));
  EXPECT_EQ(None, WorkingDirectoryState::getDriveForPath("A:\\", Style::posix));
  EXPECT_EQ(None, WorkingDirectoryState::getDriveForPath("a:\\", Style::posix));
  EXPECT_EQ(None,
            WorkingDirectoryState::getDriveForPath("A:\\abc", Style::posix));
  EXPECT_EQ(None,
            WorkingDirectoryState::getDriveForPath("A:abc", Style::posix));
  EXPECT_EQ(None, WorkingDirectoryState::getDriveForPath("L:\\", Style::posix));
  EXPECT_EQ(None, WorkingDirectoryState::getDriveForPath("Z:\\", Style::posix));

  // Check windows.
  EXPECT_EQ('A', WorkingDirectoryState::getDriveForPath("A:", Style::windows));
  EXPECT_EQ('A',
            WorkingDirectoryState::getDriveForPath("A:\\", Style::windows));
  EXPECT_EQ('A',
            WorkingDirectoryState::getDriveForPath("a:\\", Style::windows));
  EXPECT_EQ('A',
            WorkingDirectoryState::getDriveForPath("A:\\abc", Style::windows));
  EXPECT_EQ('A',
            WorkingDirectoryState::getDriveForPath("A:abc", Style::windows));
  EXPECT_EQ('L',
            WorkingDirectoryState::getDriveForPath("L:\\", Style::windows));
  EXPECT_EQ('Z',
            WorkingDirectoryState::getDriveForPath("Z:\\", Style::windows));

  // Confirm native matches the system style.
  EXPECT_EQ(WorkingDirectoryState::isValidDrive('A', system_style()),
            WorkingDirectoryState::isValidDrive('A', Style::native));
  EXPECT_EQ(WorkingDirectoryState::isValidDrive('Z', system_style()),
            WorkingDirectoryState::isValidDrive('Z', Style::native));

  // Check some paths without drives anywhere.
  for (Style PathStyle : {Style::native, Style::posix, Style::windows}) {
    for (StringRef Path : {"0:", "0:\\", "/", "/root/dir", "//server/share",
                           "//server/share/dir", "\\\\server\\share",
                           "\\\\server\\share\\dir"}) {
      EXPECT_FALSE(WorkingDirectoryState::getDriveForPath(Path, PathStyle));
    }
  }

  // Check invalid drives.
  for (Style PathStyle : {Style::native, Style::posix, Style::windows}) {
    EXPECT_FALSE(WorkingDirectoryState::isValidDrive('A' - 1, PathStyle));
    EXPECT_FALSE(WorkingDirectoryState::isValidDrive('Z' + 1, PathStyle));
    EXPECT_FALSE(WorkingDirectoryState::isValidDrive('a', PathStyle));
    EXPECT_FALSE(WorkingDirectoryState::isValidDrive('z', PathStyle));
    EXPECT_FALSE(WorkingDirectoryState::isValidDrive('0', PathStyle));
    EXPECT_FALSE(WorkingDirectoryState::isValidDrive(0, PathStyle));
  }

  // Check the member function.
  for (Style PathStyle : {Style::native, Style::posix, Style::windows}) {
    for (StringRef Path : {"a:", "a:/", "A:\\abc", "/", "0:"}) {
      EXPECT_EQ(WorkingDirectoryState::getDriveForPath(Path, PathStyle),
                WorkingDirectoryState(PathStyle).getDriveForPath(Path));
    }
  }
}

static std::string makeRoot(StringRef Path_, Style PathStyle) {
  SmallString<128> Path = Path_;
  WorkingDirectoryState::makeRoot(Path, PathStyle);
  return StringRef(Path).str();
}

TEST(WorkingDirectoryStateTest, makeRoot) {
  using T = std::tuple<StringRef, StringRef, StringRef>;
  for (auto P : {
           T{"/", "/", "\\\\.\\.\\"},
           T{"//", "//", "\\\\.\\./"},
           T{"/./", "/./", "\\\\.\\.\\./"},
           T{"/../", "/../", "\\\\.\\.\\../"},
           T{"/\\", "/\\", "\\\\.\\.\\"},
           T{"/a", "/a", "\\\\.\\.\\a"},
           T{"/./a", "/./a", "\\\\.\\.\\./a"},
           T{"/../a", "/../a", "\\\\.\\.\\../a"},
           T{"A:/ab", "/A:/ab", "A:/ab"},
           T{"a:/ab", "/a:/ab", "a:/ab"},
           T{"A:\\ab", "/A:\\ab", "A:\\ab"},
           T{"a:\\ab", "/a:\\ab", "a:\\ab"},
           T{"A:/", "/A:/", "A:/"},
           T{"a:/", "/a:/", "a:/"},
           T{"A:\\", "/A:\\", "A:\\"},
           T{"a:\\", "/a:\\", "a:\\"},
           T{"Z:/", "/Z:/", "Z:/"},
           T{"z:/", "/z:/", "z:/"},
           T{"Z:\\", "/Z:\\", "Z:\\"},
           T{"z:\\", "/z:\\", "z:\\"},
           T{"//a", "//a", "\\\\a\\."},
           T{"//a/b", "//a/b", "//a/b"},
           T{"//./.", "//./.", "//./."},
           T{"\\\\a", "/\\\\a", "\\\\a\\."},
           T{"\\\\a\\b", "/\\\\a\\b", "\\\\a\\b"},
           T{"\\\\.\\.", "/\\\\.\\.", "\\\\.\\."},
           T{"", "/", "\\\\.\\."},
           T{"./", "/./", "\\\\.\\.\\./"},
           T{"../", "/../", "\\\\.\\.\\../"},
           T{"\\", "/\\", "\\\\.\\.\\"},
           T{"\\\\", "/\\\\", "\\\\.\\.\\"},
           T{"a", "/a", "\\\\.\\.\\a"},
           T{"./a", "/./a", "\\\\.\\.\\./a"},
           T{"../a", "/../a", "\\\\.\\.\\../a"},
           T{"A:", "/A:", "A:\\"},
           T{"a:a", "/a:a", "a:\\a"},
           T{"A:a", "/A:a", "A:\\a"},
           T{"A:.", "/A:.", "A:\\."},
           T{"A:..", "/A:..", "A:\\.."},
           T{"A:a\\", "/A:a\\", "A:\\a\\"},
           T{"A:.\\", "/A:.\\", "A:\\.\\"},
           T{"A:..\\", "/A:..\\", "A:\\..\\"},
       }) {
    assert(!std::get<1>(P).empty());
    assert(!std::get<2>(P).empty());
    EXPECT_EQ(std::get<1>(P), makeRoot(std::get<0>(P), Style::posix));
    EXPECT_EQ(std::get<2>(P), makeRoot(std::get<0>(P), Style::windows));
    EXPECT_EQ(makeRoot(std::get<0>(P), system_style()),
              makeRoot(std::get<0>(P), Style::native));
  }
}

static void checkCrashWindows(Style PathStyle) {
  ASSERT_NE(Style::posix, PathStyle);

  WorkingDirectoryState WD(PathStyle);
  SmallString<128> Filename = StringRef("filename");
  EXPECT_FALSE(WD.hasDriveWorkingDirectory('A'));
  (void)WD.getDriveWorkingDirectory('A');
  WD.setDriveWorkingDirectory('A', "A:\\");
#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
  EXPECT_DEATH(WD.setCurrentWorkingDirectory("/dir"), "Expected absolute path");
  EXPECT_DEATH(WorkingDirectoryState::makeAbsolute("/dir", Filename, PathStyle),
               "Expected absolute path");
  EXPECT_DEATH(WD.setDriveWorkingDirectory('a', "A:\\"), "Invalid drive");
  EXPECT_DEATH(WD.setDriveWorkingDirectory('a', "a:\\"), "Invalid drive");
  EXPECT_DEATH(WD.hasDriveWorkingDirectory('a'), "Invalid drive");
  EXPECT_DEATH(WD.getDriveWorkingDirectory('a'), "Invalid drive");
#endif
  WorkingDirectoryState::makeAbsolute("A:\\dir", Filename, PathStyle);
  EXPECT_TRUE(WorkingDirectoryState::getDriveForPath("A:\\dir", PathStyle));
}

static void checkCrashPosix(Style PathStyle) {
  ASSERT_NE(Style::windows, PathStyle);

  WorkingDirectoryState WD(PathStyle);
  SmallString<128> Filename = StringRef("filename");
#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
  EXPECT_DEATH(WD.hasDriveWorkingDirectory('A'), "Style::windows");
  EXPECT_DEATH(WD.getDriveWorkingDirectory('A'), "Style::windows");
  EXPECT_DEATH(WD.setDriveWorkingDirectory('A', "A:\\"), "Style::windows");
  EXPECT_DEATH(WD.setCurrentWorkingDirectory("A:\\"), "Expected absolute path");
  EXPECT_DEATH(
      WorkingDirectoryState::makeAbsolute("A:\\dir", Filename, PathStyle),
      "Expected absolute path");
#endif
  WorkingDirectoryState::makeAbsolute("/dir", Filename, PathStyle);
  EXPECT_FALSE(WorkingDirectoryState::getDriveForPath("A:\\dir", PathStyle));
}

TEST(WorkingDirectoryStateTest, crash) {
#ifdef _WIN32
  checkCrashWindows(Style::native);
#else // _WIN32
  checkCrashPosix(Style::native);
#endif // _WIN32
}

TEST(WorkingDirectoryStateTest, crashPosix) { checkCrashPosix(Style::posix); }

TEST(WorkingDirectoryStateTest, crashWindows) {
  checkCrashWindows(Style::windows);
}

static void checkSetWindows(Style PathStyle) {
  ASSERT_NE(Style::posix, PathStyle);

  {
    // Set current working directory to a drive.
    WorkingDirectoryState WD(PathStyle);
    for (StringRef B : {"B:\\dir", "b:\\dir"}) {
      WD.setCurrentWorkingDirectory(B);

      EXPECT_TRUE(WD.hasDriveWorkingDirectory('B'));
      EXPECT_FALSE(WD.hasDriveWorkingDirectory('A'));

      ASSERT_TRUE(WD.hasCurrentWorkingDirectory());
      ASSERT_TRUE(WD.hasDriveWorkingDirectory('B'));
      EXPECT_EQ(B, *WD.getDriveWorkingDirectory('B'));
      EXPECT_EQ(B, *WD.getCurrentWorkingDirectory());
      EXPECT_TRUE(WD.isSmall());
    }
    std::string B = WD.getDriveWorkingDirectory('B')->str();

    // Update another drive working directory without changing the current
    // working directory.
    StringRef A = "A:\\dir";
    WD.setDriveWorkingDirectory('A', A);
    ASSERT_TRUE(WD.hasDriveWorkingDirectory('A'));
    ASSERT_EQ(A, *WD.getDriveWorkingDirectory('A'));
    // FIXME: EXPECT_EQ(A, *WD.getDriveWorkingDirectory('A'));

    ASSERT_TRUE(WD.hasDriveWorkingDirectory('B'));
    EXPECT_EQ(B, *WD.getDriveWorkingDirectory('B'));
    ASSERT_TRUE(WD.hasCurrentWorkingDirectory());
    EXPECT_EQ(B, *WD.getCurrentWorkingDirectory());
    EXPECT_TRUE(WD.isLarge());

    // Update the working directory for the current drive and check that the
    // current working directory updates.
    B = "B:\\dir\\2";
    WD.setDriveWorkingDirectory('B', B);
    ASSERT_TRUE(WD.hasDriveWorkingDirectory('B'));
    EXPECT_EQ(B, *WD.getDriveWorkingDirectory('B'));
    ASSERT_TRUE(WD.hasCurrentWorkingDirectory());
    EXPECT_EQ(B, *WD.getCurrentWorkingDirectory());

    // Set current working directory to a UNC and check that the drive working
    // directories stick around.
    for (StringRef UNC :
         {"//server/share", "\\\\server\\share", "\\\\server\\share\\dir"}) {
      WD.setCurrentWorkingDirectory(UNC);
      ASSERT_TRUE(WD.hasCurrentWorkingDirectory());
      EXPECT_EQ(UNC, *WD.getCurrentWorkingDirectory());
      EXPECT_TRUE(WD.isLarge());
      ASSERT_TRUE(WD.hasDriveWorkingDirectory('A'));
      ASSERT_TRUE(WD.hasDriveWorkingDirectory('B'));
      EXPECT_EQ(A, *WD.getDriveWorkingDirectory('A'));
      EXPECT_EQ(B, *WD.getDriveWorkingDirectory('B'));
    }
  }

  {
    // Start with a UNC and switch to a drive. Should stay small.
    for (StringRef UNC :
         {"//server/share", "\\\\server\\share", "\\\\server\\share\\dir"}) {
      WorkingDirectoryState WD(PathStyle);
      WD.setCurrentWorkingDirectory(UNC);
      ASSERT_TRUE(WD.hasCurrentWorkingDirectory());
      EXPECT_EQ(UNC, *WD.getCurrentWorkingDirectory());
      EXPECT_TRUE(WD.isSmall());

      StringRef B = "B:\\dir";
      WD.setCurrentWorkingDirectory(B);
      EXPECT_TRUE(WD.isSmall());
      ASSERT_TRUE(WD.hasCurrentWorkingDirectory());
      EXPECT_EQ(B, *WD.getCurrentWorkingDirectory());
      EXPECT_EQ(B, *WD.getDriveWorkingDirectory('B'));
    }
  }

  {
    // Start with a drive working directory and switch to a UNC. Should become
    // large.
    WorkingDirectoryState WD(PathStyle);
    StringRef B = "B:\\dir";
    WD.setCurrentWorkingDirectory(B);
    EXPECT_TRUE(WD.isSmall());
    ASSERT_TRUE(WD.hasCurrentWorkingDirectory());
    ASSERT_TRUE(WD.hasDriveWorkingDirectory('B'));
    EXPECT_EQ(B, *WD.getCurrentWorkingDirectory());
    EXPECT_EQ(B, *WD.getDriveWorkingDirectory('B'));

    StringRef UNC = "\\\\server\\share\\dir";
    WD.setCurrentWorkingDirectory(UNC);
    EXPECT_TRUE(WD.isLarge());
    ASSERT_TRUE(WD.hasCurrentWorkingDirectory());
    EXPECT_EQ(UNC, *WD.getCurrentWorkingDirectory());
    ASSERT_TRUE(WD.hasDriveWorkingDirectory('B'));
    EXPECT_EQ(B, *WD.getDriveWorkingDirectory('B'));
  }
}

static void checkSetPosix(Style PathStyle) {
  ASSERT_NE(Style::windows, PathStyle);

  WorkingDirectoryState WD(PathStyle);
  EXPECT_FALSE(WD.hasCurrentWorkingDirectory());
  WD.setCurrentWorkingDirectory("/root/dir");
  EXPECT_TRUE(WD.hasCurrentWorkingDirectory());
  EXPECT_EQ("/root/dir", *WD.getCurrentWorkingDirectory());
  WD.setCurrentWorkingDirectory("/root/2");
  EXPECT_TRUE(WD.hasCurrentWorkingDirectory());
  EXPECT_EQ("/root/2", *WD.getCurrentWorkingDirectory());
}

static void checkMakeAbsoluteWindows(Style PathStyle) {
  ASSERT_NE(Style::posix, PathStyle);

  WorkingDirectoryState WD(PathStyle);
  {
    SmallString<128> Path = StringRef("C:\\croot\\filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("C:\\croot\\filename", Path);
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("C:\\croot\\filename", Path);
  }
  {
    // Check invention of a UNC.
    SmallString<128> Path = StringRef("unc\\root\\filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Failed());
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("\\\\.\\.\\unc\\root\\filename", Path);
  }

  WD.setCurrentWorkingDirectory("A:\\aroot");
  for (StringRef Filename : {"filename", "A:filename", "a:filename"}) {
    SmallString<128> Path = Filename;
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("A:\\aroot\\filename", Path);

    Path = Filename;
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("A:\\aroot\\filename", Path);
  }
  for (StringRef Filename : {"B:filename", "b:filename"}) {
    SmallString<128> Path = Filename;
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Failed());

    Path = Filename;
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("B:\\filename", Path);
  }
  {
    SmallString<128> Path = StringRef("C:\\croot\\filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("C:\\croot\\filename", Path);

    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("C:\\croot\\filename", Path);
  }

  WD.setCurrentWorkingDirectory("B:\\broot");
  for (StringRef Filename : {"A:filename", "a:filename"}) {
    SmallString<128> Path = Filename;
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("A:\\aroot\\filename", Path);

    Path = Filename;
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("A:\\aroot\\filename", Path);
  }
  for (StringRef Filename : {"filename", "B:filename", "b:filename"}) {
    SmallString<128> Path = Filename;
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("B:\\broot\\filename", Path);

    Path = Filename;
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("B:\\broot\\filename", Path);
  }
  {
    SmallString<128> Path = StringRef("C:\\croot\\filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("C:\\croot\\filename", Path);

    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("C:\\croot\\filename", Path);
  }

  WD.setCurrentWorkingDirectory("//server/share/dir");
  for (StringRef Filename : {"A:filename", "a:filename"}) {
    SmallString<128> Path = Filename;
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("A:\\aroot\\filename", Path);

    Path = Filename;
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("A:\\aroot\\filename", Path);
  }
  for (StringRef Filename : {"B:filename", "b:filename"}) {
    SmallString<128> Path = Filename;
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("B:\\broot\\filename", Path);

    Path = Filename;
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("B:\\broot\\filename", Path);
  }
  {
    SmallString<128> Path = StringRef("C:\\croot\\filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("C:\\croot\\filename", Path);

    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("C:\\croot\\filename", Path);
  }
  {
    SmallString<128> Path = StringRef("nested\\filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("//server/share/dir\\nested\\filename", Path);
  }
}

static void checkMakeAbsolutePosix(Style PathStyle) {
  ASSERT_NE(Style::windows, PathStyle);

  WorkingDirectoryState WD(PathStyle);
  {
    SmallString<128> Path = StringRef("/root/filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("/root/filename", Path);

    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("/root/filename", Path);
  }
  {
    SmallString<128> Path = StringRef("filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Failed());
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("/filename", Path);

    Path = StringRef("./filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Failed());
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("/./filename", Path);

    Path = StringRef("../filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Failed());
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("/../filename", Path);
  }

  WD.setCurrentWorkingDirectory("/root");
  {
    SmallString<128> Path = StringRef("filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("/root/filename", Path);

    Path = StringRef("filename");
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("/root/filename", Path);
  }
  {
    // Shouldn't canonicalize.
    SmallString<128> Path = StringRef("./filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("/root/./filename", Path);

    Path = StringRef("./filename");
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("/root/./filename", Path);
  }
  {
    // Shouldn't canonicalize.
    SmallString<128> Path = StringRef("../filename");
    EXPECT_THAT_ERROR(WD.makeAbsoluteOrFail(Path), Succeeded());
    EXPECT_EQ("/root/../filename", Path);

    Path = StringRef("../filename");
    WD.makeAbsoluteOrAssumeRoot(Path);
    EXPECT_EQ("/root/../filename", Path);
  }
}

TEST(WorkingDirectoryStateTest, set) {
#ifdef _WIN32
  checkSetWindows(Style::native);
#else // _WIN32
  checkSetPosix(Style::native);
#endif // _WIN32
}

TEST(WorkingDirectoryStateTest, setPosix) { checkSetPosix(Style::posix); }

TEST(WorkingDirectoryStateTest, setWindows) { checkSetWindows(Style::windows); }

TEST(WorkingDirectoryStateTest, makeAbsolute) {
#ifdef _WIN32
  checkMakeAbsoluteWindows(Style::native);
#else  // _WIN32
  checkMakeAbsolutePosix(Style::native);
#endif // _WIN32
}

TEST(WorkingDirectoryStateTest, makeAbsolutePosix) {
  checkMakeAbsoluteWindows(Style::windows);
}

TEST(WorkingDirectoryStateTest, makeAbsoluteWindows) {
  checkMakeAbsoluteWindows(Style::windows);
}

TEST(WorkingDirectoryStateTest, move) {
  {
    WorkingDirectoryState WD1(Style::posix), WD2;
    ASSERT_EQ(Style::posix, WD1.getPathStyle());
    ASSERT_EQ(Style::native, WD2.getPathStyle());
    EXPECT_TRUE(WD1.isEmpty());
    EXPECT_TRUE(WD2.isEmpty());

    WD1.setCurrentWorkingDirectory("/root/dir");
    EXPECT_EQ(StringRef("/root/dir"), WD1.getCurrentWorkingDirectory());

    WD2 = std::move(WD1);
    EXPECT_EQ(Style::posix, WD1.getPathStyle());
    EXPECT_EQ(Style::posix, WD2.getPathStyle());
    EXPECT_TRUE(WD1.isEmpty());
    EXPECT_EQ(StringRef("/root/dir"), WD2.getCurrentWorkingDirectory());

    WorkingDirectoryState WD3(std::move(WD2));
    EXPECT_EQ(Style::posix, WD2.getPathStyle());
    EXPECT_EQ(Style::posix, WD3.getPathStyle());
    EXPECT_TRUE(WD1.isEmpty());
    EXPECT_TRUE(WD2.isEmpty());
    EXPECT_EQ(StringRef("/root/dir"), WD3.getCurrentWorkingDirectory());

    WD3 = WorkingDirectoryState();
    EXPECT_TRUE(WD3.isEmpty());
    EXPECT_EQ(Style::native, WD3.getPathStyle());
  }

  {
    WorkingDirectoryState WD1(Style::windows), WD2;
    ASSERT_EQ(Style::windows, WD1.getPathStyle());
    ASSERT_EQ(Style::native, WD2.getPathStyle());
    EXPECT_TRUE(WD1.isEmpty());
    EXPECT_TRUE(WD2.isEmpty());

    WD1.setCurrentWorkingDirectory("C:\\dir");
    EXPECT_EQ(StringRef("C:\\dir"), WD1.getCurrentWorkingDirectory());

    WD2 = std::move(WD1);
    EXPECT_EQ(Style::windows, WD1.getPathStyle());
    EXPECT_EQ(Style::windows, WD2.getPathStyle());
    EXPECT_TRUE(WD1.isEmpty());
    EXPECT_EQ(StringRef("C:\\dir"), WD2.getCurrentWorkingDirectory());

    WorkingDirectoryState WD3(std::move(WD2));
    EXPECT_EQ(Style::windows, WD2.getPathStyle());
    EXPECT_EQ(Style::windows, WD3.getPathStyle());
    EXPECT_TRUE(WD2.isEmpty());
    EXPECT_EQ(StringRef("C:\\dir"), WD3.getCurrentWorkingDirectory());

    WD3 = WorkingDirectoryState();
    EXPECT_TRUE(WD3.isEmpty());
    EXPECT_EQ(Style::native, WD3.getPathStyle());

    WD2.setCurrentWorkingDirectory("A:\\dir");
    WD2.setCurrentWorkingDirectory("C:\\dir");
    EXPECT_EQ(StringRef("A:\\dir"), WD2.getDriveWorkingDirectory('A'));
    EXPECT_EQ(StringRef("C:\\dir"), WD2.getCurrentWorkingDirectory());
    WD3 = std::move(WD2);
    EXPECT_EQ(Style::windows, WD2.getPathStyle());
    EXPECT_EQ(Style::windows, WD3.getPathStyle());
    EXPECT_TRUE(WD2.isEmpty());
    EXPECT_EQ(StringRef("A:\\dir"), WD3.getDriveWorkingDirectory('A'));
    EXPECT_EQ(StringRef("C:\\dir"), WD3.getCurrentWorkingDirectory());
  }
}

TEST(WorkingDirectoryStateTest, copy) {
  {
    WorkingDirectoryState WD1(Style::posix), WD2;
    ASSERT_EQ(Style::posix, WD1.getPathStyle());
    ASSERT_EQ(Style::native, WD2.getPathStyle());
    EXPECT_TRUE(WD1.isEmpty());
    EXPECT_TRUE(WD2.isEmpty());

    WD1.setCurrentWorkingDirectory("/root/dir");
    EXPECT_EQ(StringRef("/root/dir"), WD1.getCurrentWorkingDirectory());

    WD2 = WD1;
    EXPECT_EQ(Style::posix, WD1.getPathStyle());
    EXPECT_EQ(Style::posix, WD2.getPathStyle());
    EXPECT_EQ(StringRef("/root/dir"), WD1.getCurrentWorkingDirectory());
    EXPECT_EQ(StringRef("/root/dir"), WD2.getCurrentWorkingDirectory());

    WorkingDirectoryState WD3(WD2);
    EXPECT_EQ(Style::posix, WD2.getPathStyle());
    EXPECT_EQ(Style::posix, WD3.getPathStyle());
    EXPECT_EQ(StringRef("/root/dir"), WD2.getCurrentWorkingDirectory());
    EXPECT_EQ(StringRef("/root/dir"), WD3.getCurrentWorkingDirectory());

    WD3 = static_cast<const WorkingDirectoryState &>(WorkingDirectoryState());
    EXPECT_TRUE(WD3.isEmpty());
    EXPECT_EQ(Style::native, WD3.getPathStyle());
  }

  {
    WorkingDirectoryState WD1(Style::windows), WD2;
    ASSERT_EQ(Style::windows, WD1.getPathStyle());
    ASSERT_EQ(Style::native, WD2.getPathStyle());
    EXPECT_TRUE(WD1.isEmpty());
    EXPECT_TRUE(WD2.isEmpty());

    WD1.setCurrentWorkingDirectory("C:\\dir");
    EXPECT_EQ(StringRef("C:\\dir"), WD1.getCurrentWorkingDirectory());

    WD2 = WD1;
    EXPECT_EQ(Style::windows, WD1.getPathStyle());
    EXPECT_EQ(Style::windows, WD2.getPathStyle());
    EXPECT_EQ(StringRef("C:\\dir"), WD1.getCurrentWorkingDirectory());
    EXPECT_EQ(StringRef("C:\\dir"), WD2.getCurrentWorkingDirectory());

    WorkingDirectoryState WD3(WD2);
    EXPECT_EQ(Style::windows, WD2.getPathStyle());
    EXPECT_EQ(Style::windows, WD3.getPathStyle());
    EXPECT_EQ(StringRef("C:\\dir"), WD2.getCurrentWorkingDirectory());
    EXPECT_EQ(StringRef("C:\\dir"), WD3.getCurrentWorkingDirectory());

    WD3 = static_cast<const WorkingDirectoryState &>(WorkingDirectoryState());
    EXPECT_TRUE(WD3.isEmpty());
    EXPECT_EQ(Style::native, WD3.getPathStyle());

    WD2.setCurrentWorkingDirectory("A:\\dir");
    WD2.setCurrentWorkingDirectory("C:\\dir");
    EXPECT_EQ(StringRef("A:\\dir"), WD2.getDriveWorkingDirectory('A'));
    EXPECT_EQ(StringRef("C:\\dir"), WD2.getCurrentWorkingDirectory());
    WD3 = WD2;
    EXPECT_EQ(Style::windows, WD2.getPathStyle());
    EXPECT_EQ(Style::windows, WD3.getPathStyle());
    EXPECT_EQ(StringRef("A:\\dir"), WD2.getDriveWorkingDirectory('A'));
    EXPECT_EQ(StringRef("C:\\dir"), WD2.getCurrentWorkingDirectory());
    EXPECT_EQ(StringRef("A:\\dir"), WD3.getDriveWorkingDirectory('A'));
    EXPECT_EQ(StringRef("C:\\dir"), WD3.getCurrentWorkingDirectory());
  }
}

} // end namespace
