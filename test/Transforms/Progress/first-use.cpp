// A first-use effect blocks the same way in the program's own code and in
// the runtime's. A function-local static of the program's takes a
// static-initialisation guard and fails its promise. The spsc pop takes
// none: the runtime measures the tick rate its slip reads when the queue
// is built, so the consumer passes.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: error: wtc: own_static() is declared lock-free but can block
// CHECK-DAG: note: which takes a static-initialisation lock (__cxa_guard_acquire)
// CHECK-DAG: remark: wtc: take(int&) is declared wait-free: it is

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"
#include <chrono>

wtc::queue<int> q;

static long calibrate() { return std::chrono::steady_clock::now().time_since_epoch().count() & 1; }

WTC_LOCK_FREE long own_static() {
    static const long rate = calibrate();
    return rate;
}

WTC_WAIT_FREE bool take(int &x) { return q.try_pop(x); }

void producer() {
    for (int i = 0; i < 1000; ++i) q.push(i);
}

void consumer() {
    int x;
    while (!take(x)) {}
}

int main() {
    own_static();
    std::thread p = wtc::spawn(producer), c = wtc::spawn(consumer);
    p.join();
    c.join();
}
