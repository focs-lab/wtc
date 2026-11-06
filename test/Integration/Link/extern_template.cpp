// Several C++ files, linked as CIR modules (wtc-link): the first file
// declares an instantiation `extern template` (available externally at
// -O1, not emitted), the second defines it. The link keeps the definition,
// as the linker does; before, it kept the first and the program did not
// link.
//
// RUN: %clangxx -std=c++17 -O1 -I%S/Inputs %s %S/Inputs/extern_template_b.cpp -o %t.ref.exe
// RUN: %t.ref.exe | FileCheck %s
// RUN: %wtc_cc -O1 -I%S/Inputs %s %S/Inputs/extern_template_b.cpp -o %t.exe
// RUN: %t.exe | FileCheck %s
//
// CHECK: 42

#include "extern_template.h"
#include <cstdio>
extern template struct Box<int>;
int main() { Box<int> b{41}; std::printf("%d\n", b.get()); }
