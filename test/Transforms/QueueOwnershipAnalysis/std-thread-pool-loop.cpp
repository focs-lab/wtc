// Threads started in a counted loop through std::vector::emplace_back, as
// a thread pool starts its workers. The spawn is three calls below the
// loop, on one of two paths (construct in place, or reallocate and
// construct), each starting exactly one thread per call; the loop still
// starts one thread per iteration, and is recorded on the queue so that
// the implementation can be chosen when the queue is built.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>%t.remarks
// RUN: FileCheck %s < %t.remarks
// RUN: FileCheck %s --check-prefix=NONE < %t.remarks
//
// CHECK-DAG: queue @lambda: spmc (producers: 1, consumers: many)
// CHECK-DAG: queue @lambda: consumers: one thread per iteration of the loop at std-thread-pool-loop.cpp:{{[0-9]+}}:{{[0-9]+}}, so one when it runs at most once
// CHECK-DAG: queue @function: spmc (producers: 1, consumers: many)
// CHECK-DAG: queue @function: consumers: one thread per iteration of the loop at std-thread-pool-loop.cpp:{{[0-9]+}}:{{[0-9]+}}, so one when it runs at most once
// CHECK-DAG: queue @twice: spmc (producers: 1, consumers: many)
// NONE-NOT: queue @twice: consumers: one thread per iteration
// NONE-NOT: could not be resolved

#include "queue.hpp"

#include <thread>
#include <vector>

int N = 1;
wtc::queue<int> lambda, function, twice;

void take_function() { function.try_pop(); }
void take_twice() { twice.try_pop(); }

int main(int argc, char **) {
    N = argc;
    std::vector<std::thread> pool;
    for (int i = 0; i < N; i++) pool.emplace_back([] { lambda.try_pop(); });
    for (int i = 0; i < N; i++) pool.emplace_back(take_function);
    // Two threads per iteration: not one per iteration.
    for (int i = 0; i < N; i++) {
        pool.emplace_back(take_twice);
        pool.emplace_back(take_twice);
    }
    lambda.push(1);
    function.push(1);
    twice.push(1);
    for (auto &t : pool) t.join();
}
