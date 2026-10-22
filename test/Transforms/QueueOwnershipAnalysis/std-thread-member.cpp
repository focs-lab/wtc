// A thread started with std::thread and a pointer to member function. The
// member pointer is a private constant the function's address sits in,
// copied into a temporary and handed to the constructor; that is not an
// address taken, so the worker is not a function that may run anywhere,
// and the thread that runs it is counted.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: address of this function is taken
// CHECK-NOT: could not be resolved
// CHECK: queue @q: spsc (producers: 1, consumers: 1)
// CHECK-NOT: address of this function is taken
// CHECK-NOT: could not be resolved

#include "queue.hpp"

#include <thread>

wtc::queue<int> q;

struct worker {
    int got = 0;
    void loop() { while (got < 3) if (q.try_pop()) ++got; }
};

int main() {
    worker w;
    std::thread t(&worker::loop, &w);
    for (int i = 0; i < 3; ++i) q.push(i);
    t.join();
    return w.got == 3 ? 0 : 1;
}
