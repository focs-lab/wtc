// Several C++ files, linked as CIR modules (wtc-cc, wtc-link): an earlier
// file's overload is not this file's. Compiled apart, main's show(2.5)
// converts to int and calls its own show(int). Concatenated into one unit
// (--unity until 6 Nov 2026), the earlier file's show(double) was a better
// match, and the build had to stop; linked, each file is compiled alone
// and the program prints what it prints built apart.
//
// RUN: %clangxx -O1 -w %S/Inputs/unity_overload_a.cpp %s -o %t.exe
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift --unity -O1 -w %S/Inputs/unity_overload_a.cpp %s -o %t.u.exe
// RUN: %t.u.exe | FileCheck %s --check-prefix=OUT
//
// OUT: double 1.5
// OUT: int 2

#include <cstdio>
void a_run();
static void show(int x) { std::printf("int %d\n", x); }
int main() {
    a_run();
    show(2.5);
    return 0;
}
