// spmc with two producers is broken however many threads pop, and the
// build stops naming both.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: error: wtc: queue @q is declared spmc but 2 threads push to it
// CHECK: note: push on a thread that starts in left() at
// CHECK: note: push on a thread that starts in right() at

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q(wtc::topology::spmc);

void left() { q.push(1); }
void right() { q.push(2); }
void reader() { for (int got = 0; got < 2;) if (q.try_pop()) ++got; }

int main() {
    std::thread a = wtc::spawn(left), b = wtc::spawn(right), r = wtc::spawn(reader);
    a.join(); b.join(); r.join();
}
