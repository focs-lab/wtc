// A finding of the review of 23 Nov 2026: the program's wtc_note in another
// file. Lifted, the program printed otherwise; now the class is refused,
// and the program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_runtime_name_linked_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

#include "Inputs/lq.h"
extern "C" void wtc_note(void *p);  // f10b_other.cpp
extern "C" void wtc_report();
LQ g;
int main() {
    g.push(1); g.push(2); g.push(3);
    wtc_note(&g);
    wtc_report();
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
