// A finding of the review of 23 Nov 2026: a shared_ptr<void> cast back to
// the object's first member with std::static_pointer_cast, whose cast is in
// libstdc++'s header. Lifted, the program printed otherwise; now the class
// is refused, and the program prints what it prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

#include "Inputs/lq.h"
#include <memory>

int main() {
    std::shared_ptr<LQ> sp(new LQ);
    sp->push(1); sp->push(2); sp->push(3);
    std::shared_ptr<void> erased = sp;
    auto q = std::static_pointer_cast<std::queue<int>>(erased);
    std::printf("size %zu\n", q->size());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { sp->pop(v); s += v; }
    std::printf("sum %d\n", s);
}
