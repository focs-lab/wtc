// Once an open finding (docs/lifting.md, "Findings of 1 Nov 2026"), the
// closed world: the object's address, made void *, is the argument a
// __cxa_atexit call registers, which the exception for a global's
// destructor admits since the function registered has its body here. That
// function hands the void * on to the other file, which the pass does not
// follow; the other file, built at -O2, reads the queue with its own
// inlined pending() at exit. Lifted, it reads the substitute's pointer as
// the container. Now the escape analysis hands the registered argument to
// at_end's parameter and follows it to report_left, and the class is
// refused; the program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_atexit_forwarding_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: popped 0
// CHECK: left 2
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: at_end hands a value that may point to it to report_left, which has no body here

#include "Inputs/other_unit.h"
#include <cstdio>

extern "C" int __cxa_atexit(void (*)(void*), void*, void*);
extern "C" char __dso_handle;
extern "C" void report_left(void* q); // Inputs/escape_atexit_forwarding_b.cpp
static void at_end(void* q) { report_left(q); }

int main() {
    auto* q = new LQ<int>;
    for (int i = 0; i < 3; ++i) q->push(i);
    __cxa_atexit(at_end, q, &__dso_handle);
    int v;
    q->pop(v);
    std::printf("popped %d\n", v);
}
