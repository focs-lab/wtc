// Known open finding (docs/lifting.md, "Known open findings"), the closed
// world: a global another file may name holds a pointer to a function that
// returns the object. The queue itself is static, and the search for the
// class in the types of such globals does not enter function types. The
// other file, built at -O2, calls through the pointer and reads the queue
// with its own inlined pending(). Lifted, it reads the substitute's
// pointer as the container. The test passes once the class is refused
// here, or lifted keeps what the program prints as written.
//
// XFAIL: *
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/open_function_pointer_global_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3

#include "Inputs/other_unit.h"
#include <cstdio>

static LQ<int> gq;
static LQ<int>* get_q() { return &gq; }
LQ<int>* (*g_get)() = get_q;
std::size_t their_pending(); // Inputs/open_function_pointer_global_b.cpp

int main() {
    for (int i = 0; i < 3; ++i) gq.push(i);
    std::printf("pending %zu\n", their_pending());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { gq.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
