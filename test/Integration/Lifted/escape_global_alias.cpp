// A finding of the review of 6 Nov 2026: a global queue the other file
// reads through an alias of it. Lifted, the program printed otherwise; now
// the class is refused, and the program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_global_alias_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: the global gq_alias, which a file linked with this one names, may hold it

#include "Inputs/other_unit.h"
#include <cstdio>
extern "C" { LQ<int> gq; }
extern "C" LQ<int> gq_alias __attribute__((alias("gq")));
extern "C" std::size_t their_pending_alias();
int main() {
    for (int i = 0; i < 3; ++i) gq.push(i);
    std::printf("pending %zu\n", their_pending_alias());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { gq.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
