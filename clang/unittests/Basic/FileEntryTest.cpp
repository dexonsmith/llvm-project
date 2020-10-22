//===- unittests/Basic/FileEntryTest.cpp - Test FileEntry/FileEntryRef ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/FileEntry.h"
#include "llvm/ADT/StringMap.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace clang;

namespace {

using MapEntry = FileEntryRef::MapEntry;
using MapValue = FileEntryRef::MapValue;
using MapType = StringMap<llvm::ErrorOr<MapValue>>;

FileEntryRef addRef(MapType &M, StringRef Name, FileEntry &E) {
  return FileEntryRef(*M.insert({Name, MapValue(E)}).first);
}

TEST(FileEntryTest, FileEntryRef) {
  MapType Refs;
  FileEntry E1, E2;
  FileEntryRef R1 = addRef(Refs, "1", E1);
  FileEntryRef R2 = addRef(Refs, "2", E2);
  FileEntryRef R1Also = addRef(Refs, "1-also", E1);

  EXPECT_EQ("1", R1.getName());
  EXPECT_EQ("2", R2.getName());
  EXPECT_EQ("1-also", R1Also.getName());

  EXPECT_EQ(&E1, &R1.getFileEntry());
  EXPECT_EQ(&E2, &R2.getFileEntry());
  EXPECT_EQ(&E1, &R1Also.getFileEntry());

  const FileEntry *CE1 = R1;
  EXPECT_EQ(CE1, &E1);
}

TEST(FileEntryTest, MaybeFileEntryRef) {
  MapType Refs;
  FileEntry E1, E2;
  MaybeFileEntryRef M0;
  MaybeFileEntryRef M1 = addRef(Refs, "1", E1);
  MaybeFileEntryRef M2 = addRef(Refs, "2", E2);
  MaybeFileEntryRef M1Also = addRef(Refs, "1-also", E1);

  EXPECT_EQ(None, M0.getName());
  EXPECT_EQ(StringRef("1"), M1.getName());
  EXPECT_EQ(StringRef("2"), M2.getName());
  EXPECT_EQ(StringRef("1-also"), M1Also.getName());
  EXPECT_NE(None, M1.getName());
  EXPECT_NE(StringRef("1"), M2.getName());

  EXPECT_EQ(&E1, M1.getFileEntry());
  EXPECT_EQ(&E2, M2.getFileEntry());
  EXPECT_EQ(&E1, M1Also.getFileEntry());

  const FileEntry *CE1 = M1;
  EXPECT_EQ(CE1, &E1);
}

TEST(FileEntryTest, equals) {
  MapType Refs;
  FileEntry E1, E2;
  FileEntryRef R1 = addRef(Refs, "1", E1);
  FileEntryRef R2 = addRef(Refs, "2", E2);
  FileEntryRef R1Also = addRef(Refs, "1-also", E1);

  EXPECT_EQ(R1, &E1);
  EXPECT_EQ(&E1, R1);
  EXPECT_EQ(R1, R1Also);
  EXPECT_NE(R1, &E2);
  EXPECT_NE(&E2, R1);
  EXPECT_NE(R1, R2);

  MaybeFileEntryRef M0;
  MaybeFileEntryRef M1 = R1;
  MaybeFileEntryRef M2 = R2;
  MaybeFileEntryRef M1Also = R1Also;

  EXPECT_EQ(M0, nullptr);
  EXPECT_EQ(M0, None);
  EXPECT_EQ(nullptr, M0);
  EXPECT_EQ(None, M0);
  EXPECT_EQ(M1, &E1);
  EXPECT_EQ(&E1, M1);
  EXPECT_EQ(R1, M1);
  EXPECT_EQ(M1, R1);
  EXPECT_EQ(M1, M1Also);
  EXPECT_NE(M1, nullptr);
  EXPECT_NE(M1, None);
  EXPECT_NE(nullptr, M1);
  EXPECT_NE(None, M1);
  EXPECT_NE(M1, &E2);
  EXPECT_NE(&E2, M1);
  EXPECT_NE(M1, R2);
  EXPECT_NE(R2, M1);
  EXPECT_NE(M1, M2);
}

TEST(FileEntryTest, isSameRef) {
  MapType Refs;
  FileEntry E1, E2;
  FileEntryRef R1 = addRef(Refs, "1", E1);
  FileEntryRef R2 = addRef(Refs, "2", E2);
  FileEntryRef R1Also = addRef(Refs, "1-also", E1);

  EXPECT_TRUE(R1.isSameRef(FileEntryRef(R1)));
  EXPECT_TRUE(R1.isSameRef(FileEntryRef(R1.getMapEntry())));
  EXPECT_FALSE(R1.isSameRef(R2));
  EXPECT_FALSE(R1.isSameRef(R1Also));

  MaybeFileEntryRef M0;
  MaybeFileEntryRef M1 = R1;
  MaybeFileEntryRef M2 = R2;
  MaybeFileEntryRef M1Also = R1Also;

  EXPECT_FALSE(M0.isSameRef(M1));
  EXPECT_FALSE(M1.isSameRef(M0));
  EXPECT_TRUE(M1.isSameRef(MaybeFileEntryRef(M1)));
  EXPECT_TRUE(M1.isSameRef(MaybeFileEntryRef(M1.getMapEntry())));
  EXPECT_FALSE(M1.isSameRef(M1Also));
  EXPECT_FALSE(M1.isSameRef(M2));
  EXPECT_TRUE(M1.isSameRef(R1));
}

} // end namespace
