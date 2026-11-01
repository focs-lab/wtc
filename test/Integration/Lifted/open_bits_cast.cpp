// Known open finding (docs/lifting.md, "Known open findings"), the closed
// world: the object's address leaves as an integer through
// __builtin_bit_cast (std::bit_cast's body), which reads the bits of a
// pointer slot as an integer, a cast of the slot and a load. The "coerced"
// exception admits that load when it goes to a function of this module,
// which keeps the integer in a global; main hands it to the other file.
// Built at -O2, that file turns the integer back into the class and reads
// it with its own inlined pending(); none of its symbols names LQ. Lifted,
// it reads the substitute's pointer as the container. The test passes
// once the class is refused here, or lifted keeps what the program prints
// as written.
//
// XFAIL: *
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/open_bits_cast_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3

#include "Inputs/other_unit.h"
#include <cstdint>
#include <cstdio>

extern "C" std::size_t their_pending(std::uintptr_t q); // Inputs/open_bits_cast_b.cpp
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
