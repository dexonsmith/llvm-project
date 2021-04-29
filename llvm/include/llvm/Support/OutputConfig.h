//===- OutputConfig.h - Output configuration --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_OUTPUTCONFIG_H
#define LLVM_SUPPORT_OUTPUTCONFIG_H

#include "llvm/ADT/None.h"
#include "llvm/Support/Compiler.h"
#include <initializer_list>

namespace llvm {

class raw_ostream;

namespace vfs {

/// Set of advisory flags for backends. Both positive and negative flags can be
/// named (such as \a Text and \a NoText).
enum class OutputConfigFlag {
#define HANDLE_OUTPUT_CONFIG_FLAG(NAME, DEFAULT) NAME, No##NAME,
#include "llvm/Support/OutputConfig.def"
};

/// Full configuration for an output for use by the \a OutputBackend. Each
/// configuration flag is either \c true or \c false.
struct OutputConfig {
public:
  static void printFlag(raw_ostream &OS, OutputConfigFlag Flag);
  void print(raw_ostream &OS) const;
  void dump() const;

#define HANDLE_OUTPUT_CONFIG_FLAG(NAME, DEFAULT)                               \
  constexpr bool get##NAME() const { return NAME; }                            \
  constexpr void set##NAME(bool Value) { NAME = Value; }
#include "llvm/Support/OutputConfig.def"

  constexpr bool get(OutputConfigFlag Flag) const {
    switch (Flag) {
#define HANDLE_OUTPUT_CONFIG_FLAG(NAME, DEFAULT)                               \
  case OutputConfigFlag::NAME:                                                 \
    return NAME;                                                               \
  case OutputConfigFlag::No##NAME:                                             \
    return !NAME;
#include "llvm/Support/OutputConfig.def"
    }
  }

  /// Set \p Flag to \p Value.
  constexpr OutputConfig &set(OutputConfigFlag Flag, bool Value = true) {
    switch (Flag) {
#define HANDLE_OUTPUT_CONFIG_FLAG(NAME, DEFAULT)                               \
  case OutputConfigFlag::NAME:                                                 \
    NAME = Value;                                                              \
    break;                                                                     \
  case OutputConfigFlag::No##NAME:                                             \
    NAME = !Value;                                                             \
    break;
#include "llvm/Support/OutputConfig.def"
    }
    return *this;
  }

  constexpr OutputConfig &set(std::initializer_list<OutputConfigFlag> Flags,
                              bool Value = true) {
    for (auto Flag : Flags)
      set(Flag, Value);
    return *this;
  }

  constexpr OutputConfig()
      :
#define HANDLE_OUTPUT_CONFIG_FLAG(NAME, DEFAULT) NAME(DEFAULT),
#include "llvm/Support/OutputConfig.def"
        Unused(false) {
  }

  constexpr OutputConfig(NoneType) : OutputConfig() {}

  constexpr OutputConfig(std::initializer_list<OutputConfigFlag> OnFlags)
      : OutputConfig() {
    set(OnFlags);
  }

private:
#define HANDLE_OUTPUT_CONFIG_FLAG(NAME, DEFAULT) bool NAME : 1;
#include "llvm/Support/OutputConfig.def"

  /// Need an extra field implement OutputConfig default constructor as constexpr.
  bool LLVM_ATTRIBUTE_UNUSED Unused : 1;
};

} // namespace vfs
} // namespace llvm

#endif // LLVM_SUPPORT_OUTPUTCONFIG_H
