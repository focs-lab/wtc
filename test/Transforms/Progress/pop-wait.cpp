// A waiting pop blocks on every implementation, and a non-blocking pop of
// the same queue is as it was. The queue is built with a wait policy, the
// constructor's second argument, and the compiler still rewrites its
// topology tag, the first: one producer and one consumer, spsc. On the
// ring try_pop(T&) neither waits nor allocates, so the step that takes an
// element only if there is one passes the check; the step that waits for
// one fails it, with the wait as the reason. close(), called by the
// producer, is neither side's operation and leaves the verdict alone.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: queue @jobs: spsc (producers: 1, consumers: 1)
// CHECK-DAG: specialised: constructor tag rewritten to spsc
// CHECK-DAG: remark: wtc: take_now(int&) is declared lock-free: nothing it reaches blocks
// CHECK-DAG: error: wtc: take_waiting(int&) is declared lock-free but can block
// CHECK-DAG: note: which waits for a push or for the queue to be closed, as its wait policy says: spinning, or asleep in the kernel
// CHECK-DAG: error: wtc: take_within(int&) is declared lock-free but can block
// CHECK-DAG: note: which waits for a push, for the queue to be closed, or for its time limit, as its wait policy says: spinning, or asleep in the kernel

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <chrono>

wtc::queue<int> jobs(wtc::topology::generic, wtc::wait_policy::park);

WTC_LOCK_FREE bool take_now(int& v) { return jobs.try_pop(v); }
WTC_LOCK_FREE bool take_waiting(int& v) { return jobs.pop_wait(v); }
WTC_LOCK_FREE bool take_within(int& v) { return jobs.pop_wait_for(v, std::chrono::milliseconds(1)); }

void producer() {
    jobs.push(1);
    jobs.push(2);
    jobs.push(3);
    jobs.close();
}

void consumer() {
    int v = 0;
    while (!take_now(v)) {}
    take_waiting(v);
    take_within(v);
}

int main() {
    std::thread p = wtc::spawn(producer);
    std::thread c = wtc::spawn(consumer);
    p.join();
    c.join();
}
