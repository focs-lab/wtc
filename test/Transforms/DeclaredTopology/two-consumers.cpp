// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: error: wtc: queue @q is declared spsc_inline but 2 threads pop from it
// CHECK: note: pop on a thread that starts in left() at
// CHECK: note: pop on a thread that starts in right() at

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q(wtc::topology::spsc_inline);

void writer() { for (int i = 0; i < 10; ++i) q.push(i); }
void left() { q.try_pop(); }
void right() { q.try_pop(); }

int main() {
    std::thread w = wtc::spawn(writer), a = wtc::spawn(left), b = wtc::spawn(right);
    w.join(); a.join(); b.join();
}
