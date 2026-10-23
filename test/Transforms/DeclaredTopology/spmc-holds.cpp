// spmc promises one producer and lets any number of threads pop: one
// writer and two readers keep it, and the build goes on.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: remark: wtc: queue @q: declared spmc, verified
// CHECK-NOT: error

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q(wtc::topology::spmc);

void writer() { for (int i = 0; i < 20; ++i) q.push(i); }
void reader() { for (int got = 0; got < 10;) if (q.try_pop()) ++got; }

int main() {
    std::thread w = wtc::spawn(writer), a = wtc::spawn(reader), b = wtc::spawn(reader);
    w.join(); a.join(); b.join();
}
