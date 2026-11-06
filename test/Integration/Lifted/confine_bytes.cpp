// A finding of the review of 23 Nov 2026: the object's representation read
// as bytes: a byte view of the object may only clear, copy or free it
// whole. Lifted, the program printed otherwise; now the class is refused,
// and the program prints what it prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: map size 8
// CHECK: popped 1
// WHY: locked queue q_ in LQ not lifted:

#include "Inputs/lq.h"
#include <cstring>

LQ g;

int main() {
    g.push(1);
    const unsigned char *b = reinterpret_cast<const unsigned char *>(&g);
    std::size_t w;
    std::memcpy(&w, b + sizeof(void *), sizeof w);
    std::printf("map size %zu\n", w);
    int v;
    g.pop(v);
    std::printf("popped %d\n", v);
}
