// The run-time choice for consumers that std::vector::emplace_back starts
// in a counted loop: the spawn sits inside the library, three calls below
// the loop, and the loop is still the one the tag is chosen by. The queue
// is built in the function that holds the loop, as the choice requires.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: queue *@q: spmc (producers: 1, consumers: many)
// CHECK-DAG: specialised at run time: spsc_inline when the loop at run-time-count-std-thread.cpp:{{[0-9]+}}:{{[0-9]+}} (consumers) runs at most once, else spmc
// CHECK-NOT: not specialised

#include "queue.hpp"

#include <thread>
#include <vector>

wtc::queue<int> *q;

void take() {
    for (;;) {
        auto p = q->try_pop();
        if (p && *p == 0) return;
    }
}

int main(int argc, char **) {
    const int n = argc;
    q = new wtc::queue<int>;
    std::vector<std::thread> pool;
    for (int i = 0; i < n; i++) pool.emplace_back(take);
    for (int i = 1; i <= 10; ++i) q->push(i);
    for (int i = 0; i < n; i++) q->push(0);
    for (auto &t : pool) t.join();
    delete q;
}
