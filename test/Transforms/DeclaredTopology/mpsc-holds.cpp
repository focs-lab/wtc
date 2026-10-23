// mpsc promises one consumer and lets any number of threads push: two
// writers and one reader keep it, and the build goes on.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: remark: wtc: queue @q: declared mpsc, verified
// CHECK-NOT: error

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q(wtc::topology::mpsc);

void writer() { for (int i = 0; i < 10; ++i) q.push(i); }
void reader() { for (int got = 0; got < 20;) if (q.try_pop()) ++got; }

int main() {
    std::thread a = wtc::spawn(writer), b = wtc::spawn(writer), r = wtc::spawn(reader);
    a.join(); b.join(); r.join();
}
