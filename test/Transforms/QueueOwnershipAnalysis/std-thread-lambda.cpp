// A thread started with std::thread and a lambda. libstdc++'s constructor
// hands the callable to a bodiless _M_start_thread inside a heap-allocated
// state object, and the new thread runs that state's _M_run, which nothing
// names but the state's vtable. The entry is read from the vtable of the
// state the constructor builds, and the thread is counted: both queues
// below have one producer and one consumer, where before every std::thread
// was a spawn whose target could not be resolved.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: could not be resolved
// CHECK-DAG: queue @plain: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @two: mpsc (producers: many, consumers: 1)
// CHECK-NOT: could not be resolved

#include "queue.hpp"

#include <thread>

wtc::queue<int> plain, two;

int main() {
    std::thread t([] { plain.push(1); });
    std::thread a([] { two.push(1); });
    std::thread b([] { two.push(2); });
    while (!plain.try_pop()) {}
    for (int got = 0; got < 2;) if (two.try_pop()) ++got;
    t.join();
    a.join();
    b.join();
}
