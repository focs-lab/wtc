// A spawn that runs once in every iteration of a counted loop, in main,
// starts exactly as many threads as the loop has iterations. The count is
// still many, which holds whatever the number; the loop is recorded on the
// queue, so that the specialisation pass can choose when the queue is
// built. A loop that may stop early, or that another loop encloses, starts
// some other number of threads and is not recorded.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>%t.remarks
// RUN: FileCheck %s < %t.remarks
// RUN: FileCheck %s --check-prefix=NONE < %t.remarks
//
// CHECK-DAG: queue @counted: spmc (producers: 1, consumers: many)
// CHECK-DAG: queue @counted: consumers: one thread per iteration of the loop at counted-spawn-loop.cpp:{{[0-9]+}}:{{[0-9]+}}, so one when it runs at most once
// CHECK-DAG: queue @early: spmc (producers: 1, consumers: many)
// CHECK-DAG: queue @nested: spmc (producers: 1, consumers: many)
// NONE-NOT: queue @early: consumers: one thread per iteration
// NONE-NOT: queue @nested: consumers: one thread per iteration

#include "queue.hpp"
#include "thread.hpp"

#include <thread>
#include <vector>

int N = 1;
wtc::queue<int> counted;
wtc::queue<long> early;
wtc::queue<short> nested;

void put_counted() { counted.push(1); }
void take_counted() { counted.try_pop(); }
void put_early() { early.push(1); }
void take_early() { early.try_pop(); }
void put_nested() { nested.push(1); }
void take_nested() { nested.try_pop(); }

int main(int argc, char **) {
    N = argc;
    std::vector<std::thread> pool;
    pool.push_back(wtc::spawn(put_counted));
    for (int i = 0; i < N; i++) pool.push_back(wtc::spawn(take_counted));
    pool.push_back(wtc::spawn(put_early));
    for (int i = 0; i < N; i++) {
        if (i == 2) break;
        pool.push_back(wtc::spawn(take_early));
    }
    pool.push_back(wtc::spawn(put_nested));
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < N; i++) pool.push_back(wtc::spawn(take_nested));
    for (auto &t : pool) t.join();
}
