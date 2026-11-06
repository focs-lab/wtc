// A finding of the review of 23 Nov 2026: a union of two pointer types, the
// object's stored and its first member's read. Lifted, the program printed
// otherwise; now the class is refused, and the program prints what it
// prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

#include "Inputs/lq.h"
LQ g;
union Pun { LQ *lq; std::queue<int> *q; };
int main() {
    g.push(1); g.push(2); g.push(3);
    Pun u;
    u.lq = &g;
    std::printf("size %zu\n", u.q->size());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
