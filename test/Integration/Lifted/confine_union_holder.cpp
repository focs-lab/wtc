// A queue class inside a union, read through the union's other member: the
// program reads the words of the std::queue's storage, its deque's map
// size and how far its finish is from its start, as longs. The step to the
// other member is not a step on the class, and until 28 Nov 2026 the pass
// did not look at it (docs/lifting.md listed it as unchecked); lifted, the
// words would be the substitute's. Now a step into a union that holds the
// class, to another of its members, refuses the class, as the C pass
// refuses a struct (LiftedC/union_holder.c), and the program prints what
// it prints as written. A union read only through the class (ONLY_QUEUE)
// lifts, and so does a std::optional of the class (OPTIONAL), whose
// storage is a union of the class and an empty member: the front end
// builds the empty member with no step into it.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DONLY_QUEUE %s -o %t.only.exe 2>&1 | FileCheck %s --check-prefix=ONLY
// RUN: %t.only.exe | FileCheck %s --check-prefix=SUM --match-full-lines
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DOPTIONAL %s -o %t.optional.exe 2>&1 | FileCheck %s --check-prefix=ONLY
// RUN: %t.optional.exe | FileCheck %s --check-prefix=SUM --match-full-lines
//
// CHECK: map 8 queued 2
// CHECK: sum 1
// SUM: sum 1
// WHY: locked queue q_ in LQ not lifted: (a) main reads a union that holds LQ through another of its members
// ONLY: locked queue q_ in LQ lifted

#include "Inputs/lq.h"
#include <optional>

union View {
    LQ q;
    long words[16];
    View() : q() {}
    ~View() { q.~LQ(); }
};

#if defined(OPTIONAL)
int main() {
    std::optional<LQ> o;
    o.emplace();
    o->push(1);
    o->push(2);
    int v;
    o->pop(v);
    std::printf("sum %d\n", v);
}
#else
int main() {
    View u;
    u.q.push(1);
    u.q.push(2);
    u.q.push(3);
    int v;
    u.q.pop(v);
#ifndef ONLY_QUEUE
    // libstdc++'s deque: the map, its size, then the start and finish
    // iterators (cur, first, last, node) of four pointers each.
    std::printf("map %ld queued %ld\n", u.words[1], (u.words[6] - u.words[2]) / long(sizeof(int)));
#endif
    std::printf("sum %d\n", v);
}
#endif
