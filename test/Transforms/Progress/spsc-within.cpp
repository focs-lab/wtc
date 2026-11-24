// The spsc pop's bound counts its slip's polls as well as its pauses, 67 +
// 96 slip pauses: a promise of 100 steps fails on the bound alone.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: error: wtc: take(int&) is declared wait-free within 100 steps but may take 163 steps (67 + 96 slip pauses)
// CHECK: remark: wtc: take(int&): wait-free, at most 163 steps (67 + 96 slip pauses)

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q;

WTC_WAIT_FREE_WITHIN(100) bool take(int &x) { return q.try_pop(x); }

void producer() {
    for (int i = 0; i < 1000; ++i) q.push(i);
}

void consumer() {
    int x;
    while (!take(x)) {}
}

int main() {
    std::thread p = wtc::spawn(producer), c = wtc::spawn(consumer);
    p.join();
    c.join();
}
