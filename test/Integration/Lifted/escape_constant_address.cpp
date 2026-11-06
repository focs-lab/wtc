// A finding of the review of 6 Nov 2026: the address made a constant
// (constexpr void *) and handed to the other file. Lifted, the program
// printed otherwise; now the class is refused, and the program prints what
// it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_constant_address_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: main hands a value that may point to it to their_pending, which has no body here

#include "Inputs/other_unit.h"
#include <cstdio>
extern "C" std::size_t their_pending(void *q);
LQ<int> gq;
int main() {
    for (int i = 0; i < 3; ++i) gq.push(i);
    constexpr void *k = &gq;
    std::printf("pending %zu\n", their_pending(k));
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { gq.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
