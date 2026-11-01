// Known open finding (docs/lifting.md, "Known open findings"), the closed
// world: the other file hands over storage, the result of a call with no
// body here (a call's arguments are checked, not its result); the object
// is built in it by placement new, and the other file reads it by its own
// pointer. void * to LQ<int> * is a conversion into the types the pass
// follows, not out of them. The other file is built at -O2, and none of
// its symbols names LQ. Lifted, it reads the substitute's pointer as the
// container. The test passes once the class is refused here, or lifted
// keeps what the program prints as written.
//
// XFAIL: *
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/open_foreign_storage_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3

#include "Inputs/other_unit.h"
#include <cstdio>
#include <new>

extern "C" void* their_storage(); // Inputs/open_foreign_storage_b.cpp
extern "C" std::size_t their_pending();

int main() {
    auto* q = new (their_storage()) LQ<int>;
    for (int i = 0; i < 3; ++i) q->push(i);
    std::printf("pending %zu\n", their_pending());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q->pop(v); s += v; }
    std::printf("sum %d\n", s);
}
