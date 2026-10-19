// A deferred push is a push. Here one thread pushes one by one and another
// through a batch, and one thread pops: two producers. An analysis that
// did not count the batch would call the queue spsc and hand its single-
// producer ring to two producers.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: queue @q: mpsc (producers: many, consumers: 1)

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q;

void one_by_one() { for (int i = 0; i < 10; ++i) q.push(i); }
void batched() {
    wtc::queue<int>::batch b;
    q.batch_begin(b);
    for (int i = 0; i < 10; ++i) q.push_deferred(b, i);
    q.batch_end(b);
}
void reader() { for (int got = 0; got < 20;) if (q.try_pop()) ++got; }

int main() {
    std::thread a = wtc::spawn(one_by_one), b = wtc::spawn(batched), c = wtc::spawn(reader);
    a.join(); b.join(); c.join();
}
