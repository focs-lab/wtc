// Once an open finding (docs/lifting.md, "Findings of 1 Nov 2026"), the
// closed world: a global another file may name holds a pointer to a
// function that returns the object. The queue itself is static, and the
// search for the class in the types of such globals does not enter function
// types. The other file, built at -O2, calls through the pointer and reads
// the queue with its own inlined pending(). Lifted, it reads the
// substitute's pointer as the container. Now the escape analysis follows
// the global to get_q, whose result the other file may then read, and the
// class is refused; the program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_function_pointer_global_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: get_q, whose address may reach code the pass does not see (the global g_get, which a file linked with this one names, may hold it), may be handed it or hand it back

#include "Inputs/other_unit.h"
#include <cstdio>

static LQ<int> gq;
static LQ<int>* get_q() { return &gq; }
LQ<int>* (*g_get)() = get_q;
std::size_t their_pending(); // Inputs/escape_function_pointer_global_b.cpp

int main() {
    for (int i = 0; i < 3; ++i) gq.push(i);
    std::printf("pending %zu\n", their_pending());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { gq.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
