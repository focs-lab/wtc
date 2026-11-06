// A finding of the review of 6 Nov 2026: the address registered with
// on_exit, whose callback receives it as its second parameter and hands it
// on. Lifted, the program printed otherwise; now the class is refused, and
// the program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_on_exit_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pushed
// CHECK: pending at exit 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: at_end hands a value that may point to it to their_pending, which has no body here

#include "Inputs/other_unit.h"
#include <cstdio>
#include <cstdlib>
extern "C" std::size_t their_pending(void *q);
LQ<int> gq;
static void at_end(int, void *arg) { std::printf("pending at exit %zu\n", their_pending(arg)); }
int main() {
    for (int i = 0; i < 4; ++i) gq.push(i);
    int v;
    gq.pop(v);
    on_exit(at_end, &gq);
    std::printf("pushed\n");
}
