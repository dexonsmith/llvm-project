//===- HierarchicalTreeBuilderTest.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/CASDB.h"
#include "llvm/CAS/CASFileSystem.h"
#include "llvm/CAS/HierarchicalTreeBuilder.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::cas;

template <typename T>
static Error unwrapExpected(Expected<T> &&E, Optional<T> &O) {
  O.reset();
  if (!E)
    return E.takeError();
  O = std::move(*E);
  return Error::success();
}

template <class T>
static std::unique_ptr<T>
errorOrToPointer(ErrorOr<std::unique_ptr<T>> ErrorOrPointer) {
  if (ErrorOrPointer)
    return std::move(*ErrorOrPointer);
  return nullptr;
}

template <class T>
static std::unique_ptr<T>
expectedToPointer(Expected<std::unique_ptr<T>> ExpectedPointer) {
  if (ExpectedPointer)
    return std::move(*ExpectedPointer);
  consumeError(ExpectedPointer.takeError());
  return nullptr;
}

TEST(HierarchicalTreeBuilderTest, Flat) {
  std::unique_ptr<CASDB> CAS = createInMemoryCAS();

  auto make = [&](StringRef Content) {
    return *expectedToOptional(CAS->createBlob(Content));
  };

  HierarchicalTreeBuilder Builder;
  Builder.push(make("1"), TreeEntry::Regular, "/file1");
  Builder.push(make("1"), TreeEntry::Regular, "/1");
  Builder.push(make("2"), TreeEntry::Regular, "/2");
  Optional<TreeRef> Root = expectedToOptional(Builder.create(*CAS));
  ASSERT_TRUE(Root);

  std::unique_ptr<vfs::FileSystem> CASFS =
      expectedToPointer(createCASFileSystem(*CAS, *Root));
  ASSERT_TRUE(CASFS);

  std::unique_ptr<MemoryBuffer> F1 =
      errorOrToPointer(CASFS->getBufferForFile("/1"));
  std::unique_ptr<MemoryBuffer> F2 =
      errorOrToPointer(CASFS->getBufferForFile("2"));
  std::unique_ptr<MemoryBuffer> Ffile1 =
      errorOrToPointer(CASFS->getBufferForFile("file1"));
  ASSERT_TRUE(Ffile1);
  ASSERT_TRUE(F1);
  ASSERT_TRUE(F2);
  EXPECT_EQ("/1", F1->getBufferIdentifier());
  EXPECT_EQ("2", F2->getBufferIdentifier());
  EXPECT_EQ("file1", Ffile1->getBufferIdentifier());
  EXPECT_EQ("1", F1->getBuffer());
  EXPECT_EQ("2", F2->getBuffer());
  EXPECT_EQ("1", Ffile1->getBuffer());
}

TEST(HierarchicalTreeBuilderTest, Nested) {
  std::unique_ptr<CASDB> CAS = createInMemoryCAS();

  auto make = [&](StringRef Content) {
    return *expectedToOptional(CAS->createBlob(Content));
  };

  HierarchicalTreeBuilder Builder;
  Builder.push(make("blob2"), TreeEntry::Regular, "/d2");
  Builder.push(make("blob1"), TreeEntry::Regular, "/t1/d1");
  Builder.push(make("blob3"), TreeEntry::Regular, "/t3/d3");
  Builder.push(make("blob1"), TreeEntry::Regular, "/t3/t1nested/d1");
  Builder.push(make("blob1"), TreeEntry::Regular, "/t3/t2/d1also");
  Builder.push(make("blob2"), TreeEntry::Regular, "/t3/t2/d2");
  Optional<TreeRef> Root = expectedToOptional(Builder.create(*CAS));
  ASSERT_TRUE(Root);

  std::unique_ptr<vfs::FileSystem> CASFS =
      expectedToPointer(createCASFileSystem(*CAS, *Root));

  std::unique_ptr<MemoryBuffer> T1D1 =
      errorOrToPointer(CASFS->getBufferForFile("/t1/d1"));
  std::unique_ptr<MemoryBuffer> T1NestedD1 =
      errorOrToPointer(CASFS->getBufferForFile("t3/t1nested/d1"));
  std::unique_ptr<MemoryBuffer> T3T2D1Also =
      errorOrToPointer(CASFS->getBufferForFile("/t3/t2/d1also"));
  std::unique_ptr<MemoryBuffer> T3TD3 =
      errorOrToPointer(CASFS->getBufferForFile("t3/d3"));
  ASSERT_TRUE(T1D1);
  ASSERT_TRUE(T1NestedD1);
  ASSERT_TRUE(T3T2D1Also);
  ASSERT_TRUE(T3TD3);

  EXPECT_EQ("/t1/d1", T1D1->getBufferIdentifier());
  EXPECT_EQ("t3/t1nested/d1", T1NestedD1->getBufferIdentifier());
  EXPECT_EQ("/t3/t2/d1also", T3T2D1Also->getBufferIdentifier());
  EXPECT_EQ("t3/d3", T3TD3->getBufferIdentifier());

  EXPECT_EQ("blob1", T1D1->getBuffer());
  EXPECT_EQ("blob1", T1NestedD1->getBuffer());
  EXPECT_EQ("blob1", T3T2D1Also->getBuffer());
  EXPECT_EQ("blob3", T3TD3->getBuffer());
}

TEST(HierarchicalTreeBuilderTest, MergeDirectories) {
  std::unique_ptr<CASDB> CAS = createInMemoryCAS();

  auto make = [&](StringRef Content) {
    return cantFail(CAS->createBlob(Content));
  };

  auto createRoot = [&](StringRef Blob, StringRef Path,
                        Optional<TreeRef> &Root) {
    HierarchicalTreeBuilder Builder;
    Builder.push(make(Blob), TreeEntry::Regular, Path);
    ASSERT_THAT_ERROR(unwrapExpected(Builder.create(*CAS), Root), Succeeded());
  };

  Optional<TreeRef> Root1;
  createRoot("blob1", "/t1/d1", Root1);
  Optional<TreeRef> Root2;
  createRoot("blob2", "/t1/d2", Root2);
  Optional<TreeRef> Root3;
  createRoot("blob3", "/t1/nested/d1", Root3);

  HierarchicalTreeBuilder Builder;
  Builder.pushTreeContent(*Root1, "/");
  Builder.pushTreeContent(*Root2, "");
  Builder.pushTreeContent(*Root3, "/");
  Builder.pushTreeContent(*Root1, "");
  Builder.pushTreeContent(*Root1, "other1/nest");
  Optional<TreeRef> Root;
  ASSERT_THAT_ERROR(unwrapExpected(Builder.create(*CAS), Root), Succeeded());

  std::unique_ptr<vfs::FileSystem> CASFS =
      cantFail(createCASFileSystem(*CAS, *Root));

  std::unique_ptr<MemoryBuffer> T1D1 =
      errorOrToPointer(CASFS->getBufferForFile("/t1/d1"));
  std::unique_ptr<MemoryBuffer> T1D2 =
      errorOrToPointer(CASFS->getBufferForFile("/t1/d2"));
  std::unique_ptr<MemoryBuffer> T1NestedD1 =
      errorOrToPointer(CASFS->getBufferForFile("/t1/nested/d1"));
  std::unique_ptr<MemoryBuffer> OtherT1D1 =
      errorOrToPointer(CASFS->getBufferForFile("/other1/nest/t1/d1"));
  ASSERT_TRUE(T1D1);
  ASSERT_TRUE(T1D2);
  ASSERT_TRUE(T1NestedD1);
  ASSERT_TRUE(OtherT1D1);

  EXPECT_EQ("/t1/d1", T1D1->getBufferIdentifier());
  EXPECT_EQ("/t1/d2", T1D2->getBufferIdentifier());
  EXPECT_EQ("/t1/nested/d1", T1NestedD1->getBufferIdentifier());
  EXPECT_EQ("/other1/nest/t1/d1", OtherT1D1->getBufferIdentifier());

  EXPECT_EQ("blob1", T1D1->getBuffer());
  EXPECT_EQ("blob2", T1D2->getBuffer());
  EXPECT_EQ("blob3", T1NestedD1->getBuffer());
  EXPECT_EQ("blob1", OtherT1D1->getBuffer());
}

TEST(HierarchicalTreeBuilderTest, MergeDirectoriesConflict) {
  std::unique_ptr<CASDB> CAS = createInMemoryCAS();

  auto make = [&](StringRef Content) {
    return cantFail(CAS->createBlob(Content));
  };

  auto createRoot = [&](StringRef Blob, StringRef Path,
                        Optional<TreeRef> &Root) {
    HierarchicalTreeBuilder Builder;
    Builder.push(make(Blob), TreeEntry::Regular, Path);
    ASSERT_THAT_ERROR(unwrapExpected(Builder.create(*CAS), Root), Succeeded());
  };

  Optional<TreeRef> Root1;
  createRoot("blob1", "/t1/d1", Root1);
  Optional<TreeRef> Root2;
  createRoot("blob2", "/t1/d1", Root2);
  Optional<TreeRef> Root3;
  createRoot("blob3", "/t1/d1/nested", Root3);

  {
    HierarchicalTreeBuilder Builder;
    Builder.pushTreeContent(*Root1, "");
    Builder.pushTreeContent(*Root2, "");
    Optional<TreeRef> Root;
    EXPECT_THAT_ERROR(
        unwrapExpected(Builder.create(*CAS), Root),
        FailedWithMessage("duplicate path '/t1/d1' with different ID"));
  }
  {
    HierarchicalTreeBuilder Builder;
    Builder.pushTreeContent(*Root1, "");
    Builder.pushTreeContent(*Root3, "");
    Optional<TreeRef> Root;
    EXPECT_THAT_ERROR(unwrapExpected(Builder.create(*CAS), Root),
                      FailedWithMessage("duplicate path '/t1/d1'"));
  }
}
