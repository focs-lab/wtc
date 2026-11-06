// A finding of the review of 23 Nov 2026: a function of the program's named
// wtc_note, which keeps a view of the object as its first member: the
// runtime is known by its headers, never by its name. Lifted, the program
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

static std::queue<int> *view;
extern "C" void wtc_note(void *p) { view = static_cast<std::queue<int> *>(p); }

LQ g;

int main() {
    g.push(1); g.push(2); g.push(3);
    wtc_note(&g);
    std::printf("size %zu\n", view->size());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
