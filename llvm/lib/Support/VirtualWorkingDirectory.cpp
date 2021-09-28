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
  if (!CWD) {
    switch (S.AssumeForUnknown.NoVirtual) {
    case AWD_Live:
      return errorCodeToError(sys::fs::make_absolute(Path));
    case AWD_Root:
      sys::path::WorkingDirectoryState::makeRoot(Path,
                                                 sys::path::Style::native);
      return Error::success();
    case AWD_Error:
      return errorCodeToError(
          std::make_error_code(std::errc::no_such_file_or_directory));
    }
  }

  switch (S.AssumeForUnknown.Virtual) {
  case AWD_Error:
    return CWD->makeAbsoluteOrFail(Path);

  case AWD_Root:
    CWD->makeAbsoluteOrAssumeRoot(Path);
    return Error::success();

  case AWD_Live:
    return handleErrors(CWD->makeAbsoluteOrFail(Path),
                        [&](const ErrorInfoBase &) {
                          return errorCodeToError(sys::fs::make_absolute(Path));
                        });
  }
}

Error VirtualWorkingDirectory::initializeVirtualStateFromSnapshot() {
  if (auto ExpectedPaths = sys::fs::current_paths()) {
    CWD = std::move(*ExpectedPaths);
    return Error::success();
  } else {
    return ExpectedPaths.takeError();
  }
}

Error VirtualWorkingDirectory::setCurrentWorkingDirectory(const Twine &Path) {
  if (!hasVirtualState()) {
    switch (S.InitOnSet) {
    case IWD_Error:
      return errorCodeToError(
          std::make_error_code(std::errc::no_such_file_or_directory));

    case IWD_Live:
      return errorCodeToError(sys::fs::set_current_path(Path));

    case IWD_Empty:
      initializeVirtualState();
      break;

    case IWD_Snapshot:
      if (Error E = initializeVirtualStateFromSnapshot())
        return E;
      break;
    }
  }

  SmallString<128> AbsPath;
  Path.toVector(AbsPath);
  if (Error E = makeAbsolute(AbsPath))
    return E;

  switch (S.CanonicalizeOnSet) {
  case CWD_Raw:
    break;
  case CWD_KeepDotDot:
    sys::path::remove_dots(AbsPath, /*remove_dot_dot=*/false);
    break;
  case CWD_RemoveDotDot:
    sys::path::remove_dots(AbsPath, /*remove_dot_dot=*/true);
    break;
  }
  getVirtualState().setCurrentWorkingDirectory(AbsPath);
  return Error::success();
}
