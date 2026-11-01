// Known open finding (docs/lifting.md, "Known open findings"), the closed
// world: the object's address, made void *, is the argument a
// __cxa_atexit call registers, which the exception for a global's
// destructor admits since the function registered has its body here. That
// function hands the void * on to the other file, which the pass does not
// follow; the other file, built at -O2, reads the queue with its own
// inlined pending() at exit. Lifted, it reads the substitute's pointer as
// the container. The test passes once the class is refused here, or
// lifted keeps what the program prints as written.
//
// XFAIL: *
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/open_atexit_forwarding_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: popped 0
// CHECK: left 2

#include "Inputs/other_unit.h"
#include <cstdio>

extern "C" int __cxa_atexit(void (*)(void*), void*, void*);
extern "C" char __dso_handle;
extern "C" void report_left(void* q); // Inputs/open_atexit_forwarding_b.cpp
static void at_end(void* q) { report_left(q); }

int main() {
    auto* q = new LQ<int>;
    for (int i = 0; i < 3; ++i) q->push(i);
    __cxa_atexit(at_end, q, &__dso_handle);
    int v;
    q->pop(v);
    std::printf("popped %d\n", v);
}
