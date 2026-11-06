// A finding of the review of 23 Nov 2026: a tagged pointer in a bit-field,
// copied and read back: a bit-field is read from and written into what its
// address points to. Lifted, the program printed otherwise; now the class
// is refused, and the program prints what it prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3 tag 7
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

#include "Inputs/lq.h"
#include <cstdint>

LQ g;

struct Tagged {
    std::uintptr_t ptr : 48;
    std::uintptr_t tag : 16;
};

int main() {
    g.push(1); g.push(2); g.push(3);
    Tagged a;
    a.ptr = reinterpret_cast<std::uintptr_t>(&g);
    a.tag = 7;
    Tagged b = a;
    auto *q = reinterpret_cast<std::queue<int> *>(static_cast<std::uintptr_t>(b.ptr));
    std::printf("size %zu tag %u\n", q->size(), unsigned(b.tag));
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
