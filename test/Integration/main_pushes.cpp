// The main thread is a thread. Here main pushes and one spawned worker
// pushes, while another worker pops: two producers, one consumer. An
// analysis that discards main's accesses would call this spsc and hand the
// racing implementation to a program that visibly has two producers.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s
//
// CHECK: queue @q: mpsc (producers: many, consumers: 1)

#include "queue.hpp"
#include "thread.hpp"

#include <thread>

wtc::queue<int> q;

void producer() { for (int i = 0; i < 100; ++i) q.push(i); }
void consumer() { int got = 0; while (got < 200) if (q.try_pop()) ++got; }

int main() {
    std::thread c = wtc::spawn(consumer);
    std::thread p = wtc::spawn(producer);
    for (int i = 0; i < 100; ++i) q.push(i);
    p.join();
    c.join();
    return 0;
}
