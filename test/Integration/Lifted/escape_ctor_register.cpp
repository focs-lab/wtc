// A finding of the review of 23 Nov 2026: a constructor that hands `this`
// to another file. Lifted, the program printed otherwise; now the class is
// refused, and the program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_ctor_register_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

extern "C" void watch(void *);   // f1b_other.cpp
extern "C" void report();
#define LQ_OWN_CTOR watch(this);
#include "Inputs/lq.h"

int main() {
    LQ q;
    q.push(1); q.push(2); q.push(3);
    report();
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
