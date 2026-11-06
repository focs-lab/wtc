// Once an open finding (docs/lifting.md, "Findings of 1 Nov 2026"), the
// closed world: the other file hands over storage, the result of a call
// with no body here (a call's arguments are checked, not its result); the
// object is built in it by placement new, and the other file reads it by
// its own pointer. void * to LQ<int> * is a conversion into the types the
// pass follows, not out of them. The other file is built at -O2, and none
// of its symbols names LQ. Lifted, it reads the substitute's pointer as the
// container. Now the escape analysis takes the result of a call with no
// body here for the other file's, and the class is refused; the program
// prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_foreign_storage_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: main takes a value that may point to it from their_storage, which has no body here

#include "Inputs/other_unit.h"
#include <cstdio>
#include <new>

extern "C" void* their_storage(); // Inputs/escape_foreign_storage_b.cpp
extern "C" std::size_t their_pending();

int main() {
    auto* q = new (their_storage()) LQ<int>;
    for (int i = 0; i < 3; ++i) q->push(i);
    std::printf("pending %zu\n", their_pending());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q->pop(v); s += v; }
    std::printf("sum %d\n", s);
}
