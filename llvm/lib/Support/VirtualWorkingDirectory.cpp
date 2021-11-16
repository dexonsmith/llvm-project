//===- VirtualWorkingDirectory.cpp - Virtual working directory ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/VirtualWorkingDirectory.h"
#include "llvm/Support/FileSystem.h"

using namespace llvm;
using namespace llvm::vfs;

Error VirtualWorkingDirectory::makeAbsolute(SmallVectorImpl<char> &Path) const {
  if (!CWD)
    return errorCodeToError(sys::fs::make_absolute(Path));

  Error E = CWD->makeAbsoluteOrFail(Path);
  if (ShouldMakeAbsoluteFillInMissingDrives)
    return handleErrors(E,
                        [&](const ErrorInfoBase &) {
                          return errorCodeToError(sys::fs::make_absolute(Path));
                        });
  return E;
}

Expected<VirtualWorkingDirectory> VirtualWorkingDirectory::createVirtualSnapshot() {
  WorkingDirectoryState LiveWD;
  if (Error E = sys::fs::current_paths().moveInto(LiveWD))
    return std::move(E);
  return VirtualWorkingDirectory(std::move(LiveWD));
}

Error VirtualWorkingDirectory::getCurrentWorkingDirectory(SmallVectorImpl<char> &Path) const {
  if (M != Live) {
    if (Optional<StringRef> WD = VirtualWD.getCurrentWorkingDirectory()) {
      Path.assign(WD->begin(), WD->end());
      return Error::success();
    }
    if (M == Virtual)
      return errorCodeToError(make_error_code(std::errc::no_such_file_or_directory));

    // Fall through for Hybrid.
  }

  return errorCodeToError(sys::fs::current_path(Path));
}

Expected<std::string> VirtualWorkingDirectory::getCurrentWorkingDirectory() const {
  SmallString<128> Path;
  if (Error E = getCurrentWorkingDirectory(Path))
    return std::move(E);
  return Path.str().str();
}

Error VirtualWorkingDirectory::setCurrentWorkingDirectory(const Twine &Path) {
  if (!hasVirtualState())
    return errorCodeToError(sys::fs::set_current_path(Path));

  SmallString<128> AbsPath;
  Path.toVector(AbsPath);
  if (Error E = makeAbsolute(AbsPath))
    return E;

  bool RemoveDotDot = getEffectivePathStyle() == Style::windows;
  sys::path::remove_dots(AbsPath, /*remove_dot_dot=*/RemoveDotDot);

  getVirtualState().setCurrentWorkingDirectory(AbsPath);
  return Error::success();
}
