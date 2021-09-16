//===- llvm/Support/WorkingDirectoryState.h - Working directories -*- C++ -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_WORKINGDIRECTORYSTATE_H
#define LLVM_SUPPORT_WORKINGDIRECTORYSTATE_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/AlignOf.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Path.h"
#include <bitset>
#include <string>

namespace llvm {
namespace sys {
namespace path {

/// Represent a bitset of drives, between 'A' and 'Z'.
class DriveBitset {
public:
  bool any() const { return Drives.any(); }
  bool none() const { return Drives.none(); }

  bool test(char Drive) const {
    assert(Drive >= 'A' && Drive <= 'Z');
    return Drives.test(Drive - 'A');
  }
  void set(char Drive, bool Value = true) {
    assert(Drive >= 'A' && Drive <= 'Z');
    Drives.set(Drive - 'A', Value);
  }
  void reset(char Drive) {
    assert(Drive >= 'A' && Drive <= 'Z');
    Drives.reset(Drive - 'A');
  }

  DriveBitset() = default;
  explicit DriveBitset(uint64_t RawBits) : Drives(RawBits) {
    assert((RawBits >> 26) == 0 && "Expected 26 bits");
  }

private:
  std::bitset<26> Drives;
};

/// Class for representing working directories, potentially across drives other
/// than the current working directory.
///
/// \a Style::posix has a current working directory, similar to the result of
/// \a fs::current_path().
///
/// \a Style::windows additionally maintains working directories for each drive
/// from 'A:' through 'Z:', regardless of the current working directory. Drives
/// names are restricted to a single alphabetic character. \a isAbsolute() is
/// stricter than the free function \a is_absolute() to align with this.
class WorkingDirectoryState {
public:
  static Style getEffectivePathStyle(Style S) {
    return S == Style::native ? system_style() : S;
  }

  Style getPathStyle() const { return PathStyle; }

  Style getEffectivePathStyle() const {
    return getEffectivePathStyle(PathStyle);
  }

  /// Make an absolute path using known working directories.
  ///
  /// Unless \p AssumeRoot, return an Error when \p Path has an unknown working
  /// directory. Otherwise assume the working directory is a root path.
  Error makeAbsolute(SmallVectorImpl<char> &Path, bool AssumeRoot) const;

  /// Make an absolute path using known working directories, assuming root if
  /// the working directory for \p Path is unknown.
  void makeAbsoluteOrAssumeRoot(SmallVectorImpl<char> &Path) const {
    cantFail(makeAbsolute(Path, /*AssumeRoot=*/true));
  }

  /// Make an absolute path using known working directories, errorring if the
  /// working directory for \p Path is unknown.
  Error makeAbsoluteOrFail(SmallVectorImpl<char> &Path) const {
    return makeAbsolute(Path, /*AssumeRoot=*/false);
  }

  /// Turn \p Path into an absolute path by pretending it's at the root.
  ///
  /// \pre If \a getDriveForPath() does not return \a None for \p Path, then it
  /// has the same return for \p CWD.
  static void makeRoot(SmallVectorImpl<char> &Path, Style PathStyle);

  /// Turn \p Path into an absolute path by appending it to \p CWD.
  ///
  /// \pre \p CWD is an absolute path.
  /// \pre \a getDriveForPath() has the same return for \p CWD and \p Path, or
  /// it returns \a None for \p Path.
  static void makeAbsolute(const Twine &CWD, SmallVectorImpl<char> &Path,
                           Style PathStyle);

  bool hasCurrentWorkingDirectory() const { return CurrentDrive != 0; }

  /// If \p Path has a drive, extract it and return it. Returns 'A' for "a:"
  /// and "A:".
  static Optional<char> getDriveForPath(const Twine &Path, Style PathStyle);
  Optional<char> getDriveForPath(const Twine &Path) const {
    return getDriveForPath(Path, getPathStyle());
  }

  /// Check if a drive is valid.  \a Style::posix returns \c false. \a
  /// Style::windows returns \c false unless \p Drive is an uppercase character
  /// between 'A' and 'Z'.
  static bool isValidDrive(char Drive, Style PathStyle) {
    if (getEffectivePathStyle(PathStyle) != Style::windows)
      return false;
    return Drive >= 'A' && Drive <= 'Z';
  }
  bool isValidDrive(char Drive) const {
    return isValidDrive(Drive, getPathStyle());
  }

  /// Check if a path is a valid absolute path.
  ///
  /// For \a Style::windows, this is different from \a is_absolute().
  ///
  /// - Drive-style root names require a single, valid alphabetical drive
  ///   letter ("[a-zA-Z]:").
  /// - UNC-style root names are don't need a root directory, just a root name.
  static bool isAbsolute(const Twine &Path, Style PathStyle);
  bool isAbsolute(const Twine &Path) const {
    return isAbsolute(Path, getPathStyle());
  }

  bool hasDriveWorkingDirectory(char Drive) const;

  /// Set the current working directory to \p Path.
  ///
  /// \pre \p Path is a valid absolute path.
  void setCurrentWorkingDirectory(const Twine &Path);

  /// Set the current working directory to \p Path, calling \p MakeAbsolute if
  /// it's not absolute. Two possible implementations provided by this class
  /// are \a makeAbsoluteOrFail() and \a makeAbsoluteOrAssumeRoot(). Another
  /// one is \a sys::fs::make_absolute().
  Error setCurrentWorkingDirectory(
      const Twine &Path,
      llvm::function_ref<Error(SmallVectorImpl<char> &)> MakeAbsolute);

  /// Set the working directory for a drive. Potentially changes current
  /// working directory, only if the current drive the same as \p Drive.
  ///
  /// \pre \a getEffectivePathStyle() is \a Style::windows.
  /// \pre \p Drive is a valid Windows drive ('A' to 'Z').
  /// \pre \p Path is a valid absolute path on \p Drive.
  void setDriveWorkingDirectory(char Drive, const Twine &Path);

private:
  void setCurrentWorkingDirectoryImpl(std::string &&PathStr);
  void setLargeDriveWorkingDirectoryImpl(char Drive, std::string &&PathStr);

public:
  Optional<StringRef> getCurrentWorkingDirectory() const;

  /// Get the stored working directory for a drive, even if it's not the
  /// current working directory.
  ///
  /// \pre \a getEffectivePathStyle() is \a Style::windows.
  /// \pre \p Drive is a valid Windows drive ('A' to 'Z').
  Optional<StringRef> getDriveWorkingDirectory(char Drive) const;

private:
  Optional<StringRef> getDriveWorkingDirectoryImpl(char Drive) const;

public:
  bool isEmpty() const { return StorageState == Empty; }
  bool isSmall() const { return StorageState == Small; }
  bool isLarge() const { return StorageState == Large; }

  WorkingDirectoryState() = default;
  WorkingDirectoryState(sys::path::Style PathStyle) : PathStyle(PathStyle) {}

  WorkingDirectoryState(WorkingDirectoryState &&OtherWD) { moveFrom(OtherWD); }
  WorkingDirectoryState(const WorkingDirectoryState &OtherWD) {
    copyFrom(OtherWD);
  }
  WorkingDirectoryState &operator=(WorkingDirectoryState &&OtherWD) {
    destroyStorage();
    moveFrom(OtherWD);
    return *this;
  }
  WorkingDirectoryState &operator=(const WorkingDirectoryState &OtherWD) {
    destroyStorage();
    copyFrom(OtherWD);
    return *this;
  }

  ~WorkingDirectoryState() { destroyStorage(); }

private:
  using SmallStorageT = std::string;
  using LargeStorageT = DenseMap<char, std::string>;
  SmallStorageT &getSmallStorage() {
    return const_cast<SmallStorageT &>(
        static_cast<const WorkingDirectoryState *>(this)->getSmallStorage());
  }
  const SmallStorageT &getSmallStorage() const {
    assert(isSmall());
    return reinterpret_cast<const SmallStorageT &>(Storage);
  }
  LargeStorageT &getLargeStorage() {
    return const_cast<LargeStorageT &>(
        static_cast<const WorkingDirectoryState *>(this)->getLargeStorage());
  }
  const LargeStorageT &getLargeStorage() const {
    assert(isLarge());
    return reinterpret_cast<const LargeStorageT &>(Storage);
  }
  void createSmallStorage();
  void createLargeStorage();
  void destroyStorage();
  void moveFrom(WorkingDirectoryState &OtherWD);
  void copyFrom(const WorkingDirectoryState &OtherWD);
  void copyPODFields(const WorkingDirectoryState &OtherWD);

  /// Check for valid drive root names. \a Style::posix has none. \a
  /// Style::windows requires a single alphabetic character followed by a
  /// colon.
  static bool isValidDriveRootName(StringRef RootName, Style PathStyle);

  /// Return '/' if there is no root name or the root name is not a drive.
  /// Return the uppercase drive letter for a valid drive.
  ///
  /// Asserts if \a root_name() returns something like 'abc:', an invalid drive
  /// name.
  static char getDriveForPathImpl(StringRef Path, Style PathStyle);

  /// Indicate a working directory that doesn't use a "drive". Could be a UNC
  /// for \a Style::windows or any path for \a Style::posix.
  constexpr static const char NoDrive = '/';

  /// Current drive. Valid values include:
  /// - 0, for no working directory;
  /// - '/' for a working directory on \a Style::posix or a UNC on \a
  ///   Style::windows; and
  /// - 'A' through 'Z' for a drive on \a Style::windows.
  char CurrentDrive = 0;

  /// Path style for the working directory. Access indirectly via \a
  /// getEffectivePathStyle() to get either \a Style::windows or \a
  /// Style::posix.
  sys::path::Style PathStyle = Style::native;

  /// Type of storage.
  enum { Empty, Small, Large } StorageState = Empty;

  AlignedCharArrayUnion<SmallStorageT, LargeStorageT> Storage;
};

} // end namespace path
} // end namespace sys
} // end namespace llvm

#endif // LLVM_SUPPORT_WORKINGDIRECTORYSTATE_H
