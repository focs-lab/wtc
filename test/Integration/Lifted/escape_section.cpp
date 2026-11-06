// A finding of the review of 6 Nov 2026: the address kept in a global of a
// section of its own, which the other file walks through the linker's
// __start_ and __stop_ symbols. Lifted, the program printed otherwise; now
// the class is refused, and the program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_section_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: the global _ZL3reg, in the section wtcreg, may hold it

#include "Inputs/other_unit.h"
#include <cstdio>
LQ<int> gq;
__attribute__((used, section("wtcreg"))) static void *reg = &gq;
extern "C" std::size_t their_pending_registered();
int main() {
    for (int i = 0; i < 3; ++i) gq.push(i);
    std::printf("pending %zu\n", their_pending_registered());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { gq.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
