// Every requirement that reaches a consumer counts, not only the first: a
// consumer reached from a function declared lock-free and from a
// progress-critical one keeps the pushes of its queue's type unbatched.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-pushes --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: remark: wtc: pushes in this loop not batched: a consumer of this queue's type is reached from fast(int&), declared progress-critical

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q;

bool consume_one(int &x) { return q.try_pop(x); }

WTC_LOCK_FREE bool stats(int &x) { return consume_one(x); }
WTC_PROGRESS_CRITICAL bool fast(int &x) { return consume_one(x); }

WTC_BATCH void produce() {
    for (int i = 0; i < 1000; ++i) q.push(i);
}

void consumer() {
    int x;
    stats(x);
    fast(x);
}

int main() {
    std::thread p = wtc::spawn(produce), c = wtc::spawn(consumer);
    p.join();
    c.join();
}
