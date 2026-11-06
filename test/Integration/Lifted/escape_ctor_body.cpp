// A finding of the review of 23 Nov 2026: a constructor's body, which keeps
// a view of the object as its first member: the analysis walks constructors
// and destructors, since the rewrite replaces only the container's
// construction in them. Lifted, the program printed otherwise; now the
// class is refused, and the program prints what it prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

#include <queue>
std::queue<int> *view;
#define LQ_OWN_CTOR view = reinterpret_cast<std::queue<int> *>(this);
#include "Inputs/lq.h"

LQ g;

int main() {
    g.push(1); g.push(2); g.push(3);
    std::printf("size %zu\n", view->size());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
