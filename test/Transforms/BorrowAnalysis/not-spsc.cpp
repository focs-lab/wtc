// Only a queue proved spsc is a candidate: with two consumers there is
// nothing to borrow from, whatever the consumers do.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis 2>&1 >/dev/null | FileCheck %s
//
// CHECK: queue @q: spmc
// CHECK-NOT: borrow

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q;
int sum = 0;

void produce() { for (int i = 0; i < 100; ++i) q.push(i); }
void consume() { for (int i = 0; i < 50; ++i) if (auto p = q.try_pop()) sum += *p; }

int main() {
    std::thread a = wtc::spawn(produce), b = wtc::spawn(consume), c = wtc::spawn(consume);
    a.join(); b.join(); c.join();
    return sum & 1;
}
