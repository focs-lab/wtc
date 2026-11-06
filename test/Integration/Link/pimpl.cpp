// Several C++ files, linked as CIR modules (wtc-link): the pimpl idiom,
// class Impl complete only in the second file (as pimpl.c).
//
// RUN: %clangxx -std=c++17 -O1 -I%S/Inputs %s %S/Inputs/pimpl_b.cpp -o %t.ref.exe
// RUN: %t.ref.exe | FileCheck %s
// RUN: %wtc_cc -O1 -I%S/Inputs %s %S/Inputs/pimpl_b.cpp -o %t.exe
// RUN: %t.exe | FileCheck %s
//
// CHECK: 42

#include <cstdio>
#include "pimpl.hpp"
int main() { Widget w; std::printf("%d\n", w.value()); }
