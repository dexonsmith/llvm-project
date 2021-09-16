//===- WorkingDirectoryState.cpp - Working directories --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/WorkingDirectoryState.h"

using namespace llvm;
using namespace llvm::sys;
using namespace llvm::sys::path;

void WorkingDirectoryState::createSmallStorage() {
  assert(isEmpty());
  new (&Storage) SmallStorageT();
  StorageState = Small;
}

void WorkingDirectoryState::createLargeStorage() {
  assert(!isLarge());
  assert(getEffectivePathStyle() == Style::windows);

  // Save current working directory and destroy small storage.
  Optional<SmallStorageT> Existing;
  if (isSmall()) {
    assert(CurrentDrive &&
           "Expected current working directory in small storage");
    Existing = std::move(getSmallStorage());
    getSmallStorage().~SmallStorageT();
  }

  // Create large storage.
  new (&Storage) LargeStorageT();
  StorageState = Large;

  // Save current working directory.
  if (Existing)
    getLargeStorage()[CurrentDrive] = std::move(*Existing);
}

void WorkingDirectoryState::destroyStorage() {
  if (isEmpty())
    return;

  if (isSmall())
    getSmallStorage().~SmallStorageT();
  else
    getLargeStorage().~LargeStorageT();

  StorageState = Empty;
  CurrentDrive = 0;
}

void WorkingDirectoryState::moveFrom(WorkingDirectoryState &OtherWD) {
  assert(isEmpty());

  PathStyle = OtherWD.PathStyle;
  CurrentDrive = OtherWD.CurrentDrive;
  if (OtherWD.isEmpty())
    return;
  if (OtherWD.isSmall()) {
    createSmallStorage();
    getSmallStorage() = std::move(OtherWD.getSmallStorage());
  } else {
    createLargeStorage();
    getLargeStorage() = std::move(OtherWD.getLargeStorage());
  }
  assert(StorageState == OtherWD.StorageState);
  OtherWD.destroyStorage();
}

void WorkingDirectoryState::copyFrom(const WorkingDirectoryState &OtherWD) {
  assert(isEmpty());

  PathStyle = OtherWD.PathStyle;
  CurrentDrive = OtherWD.CurrentDrive;
  if (OtherWD.isEmpty())
    return;
  if (OtherWD.isSmall()) {
    createSmallStorage();
    getSmallStorage() = OtherWD.getSmallStorage();
  } else {
    createLargeStorage();
    getLargeStorage() = OtherWD.getLargeStorage();
  }
  assert(StorageState == OtherWD.StorageState);
}

static void makeAbsoluteImpl(SmallVectorImpl<char> &AbsPath,
                             SmallVectorImpl<char> &Path, Style PathStyle) {
  StringRef PathRef(Path.data(), Path.size());
  path::append(AbsPath, PathStyle, path::relative_path(PathRef, PathStyle));
  AbsPath.swap(Path);
}

static void makeAbsoluteImpl(StringRef CWD, SmallVectorImpl<char> &Path,
                             Style PathStyle) {
  SmallString<256> AbsPath = CWD;
  makeAbsoluteImpl(AbsPath, Path, PathStyle);
}

static void makeRootImpl(Optional<char> Drive, SmallVectorImpl<char> &Path,
                         Style PathStyle) {
  StringRef PathRef(Path.data(), Path.size());
  SmallString<256> AbsPath;
  if (WorkingDirectoryState::getEffectivePathStyle(PathStyle) ==
      sys::path::Style::posix) {
    AbsPath.push_back('/');
  } else if (Drive) {
    AbsPath.push_back(*Drive);
    AbsPath.append(":\\");
  } else if (PathRef.size() >= 3 && (PathRef[0] == '/' || PathRef[0] == '\\') &&
             PathRef[0] == Path[1] && has_root_name(PathRef, PathStyle) &&
             !has_root_directory(PathRef, PathStyle)) {
    // Have "//root" already. Append "." as a root directory.
    Path[0] = Path[1] = '\\';
    append(Path, PathStyle, ".");
    return;
  } else {
    // No net name. Prepend "//./.".
    AbsPath.append("\\\\.\\.");
  }
  if (Path.empty())
    AbsPath.swap(Path);
  else
    makeAbsoluteImpl(AbsPath, Path, PathStyle);
}

void WorkingDirectoryState::makeRoot(SmallVectorImpl<char> &Path,
                                     Style PathStyle) {
  StringRef PathRef(Path.data(), Path.size());
  if (isAbsolute(PathRef, PathStyle))
    return;

  Optional<char> Drive = getDriveForPath(PathRef, PathStyle);

  // Send a case-matched drive into makeRootImpl, rather than the canonical
  // one.
  if (Drive && *Drive != PathRef[0]) {
    assert(PathRef[0] >= 'a' && PathRef[0] <= 'z' &&
           "Expected lowercase drive in Path");
    Drive = 'a';
  }
  makeRootImpl(Drive, Path, PathStyle);
}

void WorkingDirectoryState::makeAbsolute(const Twine &CWD,
                                         SmallVectorImpl<char> &Path,
                                         Style PathStyle) {
  StringRef PathRef(Path.data(), Path.size());

  // Avoid initialiazing AbsPath when assertions are off if Path is already
  // absolute.
  Optional<SmallString<256>> AbsPath;
  auto initAbsPathWithCWDOnce = [&]() {
    if (AbsPath)
      return;
    AbsPath.emplace();
    CWD.toVector(*AbsPath);
  };

  // Check that CWD is a valid working directory for Path.
#ifndef NDEBUG
  initAbsPathWithCWDOnce();
  assert(isAbsolute(*AbsPath, PathStyle) && "Expected absolute path as CWD");
  if (Optional<char> Drive = getDriveForPath(PathRef, PathStyle)) {
    Optional<char> DriveCWD = getDriveForPath(*AbsPath, PathStyle);
    assert(DriveCWD && *DriveCWD == *Drive && "Invalid CWD for Path");
  }
#endif

  if (isAbsolute(PathRef, PathStyle))
    return;

  makeAbsoluteImpl(*AbsPath, Path, PathStyle);
}

Error WorkingDirectoryState::makeAbsolute(SmallVectorImpl<char> &Path,
                                          bool AssumeRoot) const {
  StringRef PathRef(Path.data(), Path.size());

  // Check for a path that's already absolute.
  if (isAbsolute(PathRef))
    return Error::success();

  Optional<char> Drive = getDriveForPath(PathRef);
  Optional<StringRef> CWD =
      Drive ? getDriveWorkingDirectory(*Drive) : getCurrentWorkingDirectory();
  if (CWD) {
    makeAbsoluteImpl(*CWD, Path, getPathStyle());
    return Error::success();
  }

  if (!AssumeRoot)
    return errorCodeToError(std::make_error_code(std::errc::no_such_device));

  makeRootImpl(Drive, Path, getPathStyle());
  return Error::success();
}

bool WorkingDirectoryState::isAbsolute(const Twine &Path_, Style PathStyle) {
  SmallString<128> Storage;
  StringRef Path = Path_.toStringRef(Storage);
  if (Path.empty())
    return false;
  if (getEffectivePathStyle(PathStyle) == Style::posix)
    return Path[0] == '/';

  // Windows absolute path needs at least three characters.
  if (Path.size() < 3)
    return false;

  switch (Path[1]) {
  case ':':
    // Check for a drive-based absolute path (e.g., "D:/" or "D:\\"). This is
    // more strict than is_absolute().
    return (Path[2] == '/' || Path[2] == '\\') &&
           ((Path[0] >= 'a' && Path[0] <= 'z') ||
            (Path[0] >= 'A' && Path[0] <= 'Z'));
  case '/':
  case '\\':
    // Check for the start of a UNC net name (e.g., "//server" or
    // "\\\\server"), then defer to is_absolute().
    return Path[0] == Path[1] && is_absolute(Path, PathStyle);
  default:
    return false;
  }
}

Optional<StringRef>
WorkingDirectoryState::getDriveWorkingDirectory(char Drive) const {
  assert(getEffectivePathStyle() == Style::windows);
  assert(isValidDrive(Drive) && "Invalid drive");
  return getDriveWorkingDirectoryImpl(Drive);
}

Optional<StringRef>
WorkingDirectoryState::getDriveWorkingDirectoryImpl(char Drive) const {
  if (isEmpty())
    return None;
  if (isSmall()) {
    if (CurrentDrive == Drive)
      return StringRef(getSmallStorage());
    return None;
  }
  auto I = getLargeStorage().find(Drive);
  if (I == getLargeStorage().end())
    return None;
  return StringRef(I->second);
}

Optional<StringRef> WorkingDirectoryState::getCurrentWorkingDirectory() const {
  if (isEmpty())
    return None;

  return getDriveWorkingDirectoryImpl(CurrentDrive);
}

void WorkingDirectoryState::setDriveWorkingDirectory(char Drive,
                                                     const Twine &Path) {
  assert(getEffectivePathStyle() == Style::windows);
  std::string PathStr = Path.str();
  assert(isAbsolute(PathStr) && "Expected absolute path");
  assert(isValidDrive(Drive) && "Invalid drive");
#ifndef NDEBUG
  Optional<char> PathDrive = getDriveForPath(PathStr);
  assert(PathDrive && "Invalid drive in path");
  assert(*PathDrive == Drive && "Mismatched drive");
#endif
  if (Drive == CurrentDrive)
    setCurrentWorkingDirectoryImpl(std::move(PathStr));
  else
    setLargeDriveWorkingDirectoryImpl(Drive, std::move(PathStr));
}

void WorkingDirectoryState::setCurrentWorkingDirectoryImpl(
    std::string &&PathStr) {
  char Drive = getDriveForPathImpl(PathStr, getPathStyle());

  // No current working directory or drive working directories.
  if (isEmpty()) {
    createSmallStorage();
    CurrentDrive = NoDrive;
    // Fallthrough to update to small storage.
  }

  // Current working directory, but either it doesn't have a drive, or it
  // does and the new one matches.
  if (isSmall() && (CurrentDrive == NoDrive || CurrentDrive == Drive)) {
    CurrentDrive = Drive;
    getSmallStorage() = std::move(PathStr);
    return;
  }

  // There are drive working directories to remember.
  assert(getEffectivePathStyle() == Style::windows &&
         "Expected a drive only on Windows");
  setLargeDriveWorkingDirectoryImpl(Drive, std::move(PathStr));
  CurrentDrive = Drive;
}

void WorkingDirectoryState::setLargeDriveWorkingDirectoryImpl(
    char Drive, std::string &&PathStr) {
  if (!isLarge())
    createLargeStorage();
  getLargeStorage()[Drive] = std::move(PathStr);
}

Optional<char> WorkingDirectoryState::getDriveForPath(const Twine &Path,
                                                      Style PathStyle) {
  SmallString<256> PathStorage;
  char Drive = getDriveForPathImpl(Path.toStringRef(PathStorage), PathStyle);
  if (Drive == NoDrive)
    return None;
  return Drive;
}

bool WorkingDirectoryState::hasDriveWorkingDirectory(char Drive) const {
  assert(getEffectivePathStyle() == Style::windows);
  assert(isValidDrive(Drive) && "Invalid drive");
  if (CurrentDrive == Drive)
    return true;
  if (!isLarge())
    return false;
  return getLargeStorage().count(Drive);
}

/// Set the current working directory to \p Path.
///
/// \pre \p Path is a valid absolute path.
void WorkingDirectoryState::setCurrentWorkingDirectory(const Twine &Path) {
  std::string PathStr = Path.str();
  assert(isAbsolute(PathStr) && "Expected absolute path");
  setCurrentWorkingDirectoryImpl(std::move(PathStr));
}

Error WorkingDirectoryState::setCurrentWorkingDirectory(
    const Twine &Path,
    llvm::function_ref<Error(SmallVectorImpl<char> &)> MakeAbsolute) {
  if (isAbsolute(Path)) {
    setCurrentWorkingDirectory(Path);
    return Error::success();
  }
  SmallString<128> AbsPath;
  Path.toVector(AbsPath);
  if (Error E = MakeAbsolute(AbsPath))
    return E;
  setCurrentWorkingDirectory(AbsPath);
  return Error::success();
}

bool WorkingDirectoryState::isValidDriveRootName(StringRef RootName,
                                                 Style PathStyle) {
  if (getEffectivePathStyle(PathStyle) != Style::windows)
    return false;
  if (RootName.size() != 2 || RootName[1] != ':')
    return false;
  char Drive = RootName[0];
  return (Drive >= 'A' && Drive <= 'Z') || (Drive >= 'a' && Drive <= 'z');
}

char WorkingDirectoryState::getDriveForPathImpl(StringRef Path,
                                                Style PathStyle) {
  StringRef RootName = root_name(Path, PathStyle);
  if (!isValidDriveRootName(RootName, PathStyle))
    return NoDrive;
  char Drive = Path[0];
  if (Drive >= 'a' && Drive <= 'z')
    Drive = Drive - 'a' + 'A';
  assert(isValidDrive(Drive, PathStyle) && "Bad drive math?");
  return Drive;
}
