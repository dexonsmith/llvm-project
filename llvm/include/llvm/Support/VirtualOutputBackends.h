//===- VirtualOutputBackends.h - Virtual output backends --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_VIRTUALOUTPUTBACKENDS_H
#define LLVM_SUPPORT_VIRTUALOUTPUTBACKENDS_H

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/VirtualOutputConfig.h"

namespace llvm {
namespace vfs {

/// Create a backend that ignores all output.
IntrusiveRefCntPtr<OutputBackend> makeNullOutputBackend();

/// An output backend that creates files on disk, wrapping APIs in sys::fs.
class OnDiskOutputBackend : public OutputBackend {
  void anchor() override;

protected:
  IntrusiveRefCntPtr<OutputBackend> cloneImpl() const override {
    return clone();
  }

  Expected<std::unique_ptr<OutputFileImpl>>
  createFileImpl(StringRef Path, Optional<OutputConfig> Config) override;

public:
  /// Resolve an absolute path.
  Error makeAbsolute(SmallVectorImpl<char> &Path) const;

  /// On disk output settings.
  struct OutputSettings {
    /// Register output files to be deleted if a signal is received. Also
    /// disabled for outputs with \a OutputConfig::getNoCrashCleanup().
    bool DisableRemoveOnSignal = false;

    /// Disable temporary files. Also disabled for outputs with \a
    /// OutputConfig::getNoAtomicWrite().
    bool DisableTemporaries = false;

    // Default configuration.
    OutputConfig DefaultConfig;
  };

  IntrusiveRefCntPtr<OnDiskOutputBackend> clone() const {
    auto Clone = makeIntrusiveRefCnt<OnDiskOutputBackend>();
    Clone->Settings = Settings;
    return Clone;
  }

  OnDiskOutputBackend() = default;

  /// Settings for this backend.
  ///
  /// Access is not thread-safe.
  OutputSettings Settings;
};

} // namespace vfs
} // namespace llvm

#endif // LLVM_SUPPORT_VIRTUALOUTPUTBACKENDS_H
