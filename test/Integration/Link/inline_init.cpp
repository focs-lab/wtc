// Several C++ files, linked as CIR modules (wtc-link): an inline variable
// and a template's static member, each with a dynamic initialiser, are
// initialised once for the program. The front end emits each file's
// initialiser with no guard, so the link keeps only the initialiser of the
// unit whose definition it keeps; before, each ran once per file.
//
// RUN: %clangxx -std=c++17 -O1 -I%S/Inputs %s %S/Inputs/inline_init_b.cpp -o %t.ref.exe
// RUN: %t.ref.exe | FileCheck %s
// RUN: %wtc_cc -O0 -I%S/Inputs %s %S/Inputs/inline_init_b.cpp -o %t.0.exe
// RUN: %t.0.exe | FileCheck %s
// RUN: %wtc_cc -O1 -I%S/Inputs %s %S/Inputs/inline_init_b.cpp -o %t.1.exe
// RUN: %t.1.exe | FileCheck %s
//
// CHECK: counter 2 runs 2 noisy 1

#include "inline_init.h"
int main() {
    ++counter();
    bump_b();
    (void)Holder<int>::n.v;
    std::printf("counter %d runs %d noisy %d\n", counter(), runs, shared_noisy.v);
}
