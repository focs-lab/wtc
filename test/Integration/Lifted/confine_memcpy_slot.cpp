// A finding of the review of 23 Nov 2026: a pointer to the object copied
// with memcpy into a pointer to its first member. Lifted, the program
// printed otherwise; now the class is refused, and the program prints what
// it prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

#include "Inputs/lq.h"
#include <cstring>
LQ g;
int main() {
    g.push(1); g.push(2); g.push(3);
    LQ *p = &g;
    std::queue<int> *q;
    std::memcpy(&q, &p, sizeof q);
    std::printf("size %zu\n", q->size());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
