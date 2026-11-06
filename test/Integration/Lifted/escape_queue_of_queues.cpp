// A finding of the review of 6 Nov 2026: a queue of void * holding another
// queue's address, popped into a local and handed to the other file: a
// pop's out-parameter receives the elements. Lifted, the program printed
// otherwise; now the class is refused, and the program prints what it
// prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_queue_of_queues_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: done
// WHY: locked queue q_ in LQ<void *> not lifted: (f) an object of LQ<void *> may be reached by code the pass does not see: main hands a value that may point to it to their_pending, which has no body here

#include "Inputs/other_unit.h"
#include <cstdio>
extern "C" std::size_t their_pending(void *q);
int main() {
    LQ<void *> qa, qb;
    for (int i = 0; i < 3; ++i) qb.push(nullptr);
    qa.push(&qb);
    void *out = nullptr;
    qa.pop(out);
    std::printf("pending %zu\n", their_pending(out));
    for (int i = 0; i < 3; ++i) qb.pop(out);
    std::printf("done\n");
}
