// Once an open finding (docs/lifting.md, "Findings of 1 Nov 2026"), the
// closed world: the object's address leaves as an integer through
// __builtin_bit_cast (std::bit_cast's body), which reads the bits of a
// pointer slot as an integer, a cast of the slot and a load. The "coerced"
// exception admits that load when it goes to a function of this module,
// which keeps the integer in a global; main hands it to the other file.
// Built at -O2, that file turns the integer back into the class and reads
// it with its own inlined pending(); none of its symbols names LQ. Lifted,
// it reads the substitute's pointer as the container. Now the escape
// analysis follows the address through the bits and the integer to the call
// of their_pending, and the class is refused; the program prints what it
// prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_bits_cast_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: main hands a value that may point to it to their_pending, which has no body here

#include "Inputs/other_unit.h"
#include <cstdint>
#include <cstdio>

extern "C" std::size_t their_pending(std::uintptr_t q); // Inputs/escape_bits_cast_b.cpp
std::uintptr_t g_bits;
void keep(std::uintptr_t b) { g_bits = b; }

int main() {
    LQ<int> q;
    for (int i = 0; i < 3; ++i) q.push(i);
    LQ<int>* p = &q;
    keep(__builtin_bit_cast(std::uintptr_t, p));
    std::printf("pending %zu\n", their_pending(g_bits));
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
