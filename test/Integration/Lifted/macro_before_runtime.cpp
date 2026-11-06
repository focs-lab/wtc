// The runtime's header comes before the program's file in the unit wtc-cc
// compiles for a class that lifts, so that no macro of the program's
// reaches it: here copy_ok, a name the runtime uses for one of its own
// functions. After the program's file, the runtime did not compile.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=LIFT
// RUN: %t.exe | FileCheck %s
//
// LIFT: locked queue q_ in LQ<int> lifted (
// CHECK: sum 3

#include "Inputs/other_unit.h"
#include <cstdio>

#define copy_ok(x) static_assert(false, "the program's macro reached the runtime")

int main() {
    LQ<int> q;
    for (int i = 0; i < 3; ++i) q.push(i);
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
