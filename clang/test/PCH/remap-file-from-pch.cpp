// RUN: %clang_cc1 -x c++-header %s.h -emit-pch -o %t.pch
// RUN: not %clang_cc1 %s -include-pch %t.pch -fsyntax-only 2>&1 \
// RUN:   | FileCheck %s -check-prefix=NO-REMAP
// RUN: not %clang_cc1 %s -include-pch %t.pch -remap-file "%s.h;%s.remap.h" -fsyntax-only 2>&1 \
// RUN:   | FileCheck %s -check-prefix=MISMATCH
//
// RUN: %clang_cc1 -x c++-header -remap-file "%s.h;%s.remap.h" %s.h -emit-pch -o %t-remapped.pch
// RUN: %clang_cc1 %s -include-pch %t-remapped.pch -remap-file "%s.h;%s.remap.h" -fsyntax-only 2>&1 \
// RUN:   | FileCheck %s -check-prefix=REMAP -allow-empty
// RUN: not %clang_cc1 %s -include-pch %t-remapped.pch -fsyntax-only 2>&1 \
// RUN:   | FileCheck %s -check-prefix=MISMATCH

const char *str = STR;
int ge = zool;

// MISMATCH: file '{{.*[/\\]}}remap-file-from-pch.cpp.h' has been modified since the precompiled header
// NO-REMAP: use of undeclared identifier 'zool'
// REMAP-NOT: use of undeclared identifier
