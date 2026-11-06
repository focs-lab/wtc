// A finding of the review of 6 Nov 2026: a function returning the address,
// called by the other file under an alias. Lifted, the program printed
// otherwise; now the class is refused, and the program prints what it
// prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_function_alias_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: a file linked with this one names get_q, an alias of get_q_impl, which may be handed it

#include "Inputs/other_unit.h"
#include <cstdio>
LQ<int> gq;
extern "C" void *get_q_impl() { return &gq; }
extern "C" void *get_q() __attribute__((alias("get_q_impl")));
extern "C" std::size_t their_pending_of_get_q();
int main() {
    for (int i = 0; i < 3; ++i) gq.push(i);
    std::printf("pending %zu\n", their_pending_of_get_q());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { gq.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
