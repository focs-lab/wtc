// A queue on the heap is the object its allocation made, and its verdict
// is kept on the constructor call that follows the allocation. All objects
// one allocation site makes are one queue to the analysis, so a site in a
// loop is counted with the threads of all its objects; here it runs once.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=CC
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK-NOT: unresolvable
// CHECK: queue *@q: spsc (producers: 1, consumers: 1)
// CHECK: specialised: constructor tag rewritten to spsc
// CC: tag rewritten to spsc_inline
// OUT: tag 2 got 10

#include "queue.hpp"
#include "thread.hpp"

#include <cstdio>

wtc::queue<int>* q;

void producer() { for (int i = 0; i < 10; ++i) q->push(i); }

int main() {
    q = new wtc::queue<int>;
    std::thread t = wtc::spawn(producer);
    int got = 0;
    while (got < 10)
        if (q->try_pop()) ++got;
    t.join();
    std::printf("tag %d got %d\n", static_cast<int>(q->tag()), got);
    delete q;
}
