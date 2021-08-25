//===- VirtualOutputFile.cpp - Output file virtualization -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/VirtualOutputFile.h"
#include "llvm/Support/VirtualOutputError.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::vfs;

void OutputFileImpl::anchor() {}

Error OutputFile::keep() {
  if (!Impl)
    return make_error<OutputError>(getPath(), OutputErrorCode::already_closed);

  Error E = Impl->keep();
  Impl = nullptr;
  DiscardOnDestroyHandler = nullptr;
  return E;
}

Error OutputFile::discard() {
  if (!Impl)
    return make_error<OutputError>(getPath(), OutputErrorCode::already_closed);

  Error E = Impl->discard();
  Impl = nullptr;
  DiscardOnDestroyHandler = nullptr;
  return E;
}

void OutputFile::destroy() {
  if (!Impl)
    return;

  // Clean up the file. Move the discard handler into a local since discard
  // will reset it.
  auto DiscardHandler = std::move(DiscardOnDestroyHandler);
  Error E = discard();
  assert(!Impl && "Expected discard to destroy Impl");

  // If there's no handler, report a fatal error.
  if (!DiscardHandler)
    llvm::report_fatal_error(joinErrors(
        make_error<OutputError>(getPath(), OutputErrorCode::not_closed),
        std::move(E)));
  else if (E)
    DiscardHandler(std::move(E));
}
