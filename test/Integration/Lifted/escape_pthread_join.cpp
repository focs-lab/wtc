// A finding of the review of 6 Nov 2026: the object's address comes back as
// a thread's result through pthread_join: its out-parameter receives what
// the thread's function returns. Lifted, the program printed otherwise; now
// the class is refused, and the program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_pthread_join_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: main hands a value that may point to it to their_pending, which has no body here

#include "Inputs/other_unit.h"
#include <cstdio>
#include <pthread.h>
extern "C" std::size_t their_pending(void *q);
static LQ<int> *g_made;
static void *worker(void *) { return g_made; }
int main() {
    LQ<int> q;
    for (int i = 0; i < 3; ++i) q.push(i);
    g_made = &q;
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
    void *r;
    pthread_join(t, &r);
    std::printf("pending %zu\n", their_pending(r));
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
