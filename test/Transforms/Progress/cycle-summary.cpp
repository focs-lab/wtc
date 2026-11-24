// What a loop's exit depends on is read from the functions it calls, and a
// summary computed while a function of its call cycle is still being read is
// not remembered as final: both loops spin on what f reads, whichever is
// read first.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: error: wtc: w1() is declared lock-free but can block
// CHECK-DAG: error: wtc: w2() is declared lock-free but can block

#include "progress.hpp"
#include <atomic>

std::atomic<bool> ready{false};
bool f(int n);
bool g(int n) { return n > 0 && f(n - 1); }
bool f(int n) {
    if (n == 0) return ready.load(std::memory_order_acquire);
    return g(n);
}

WTC_LOCK_FREE void w1() { while (!f(3)) {} }
WTC_LOCK_FREE void w2() { while (!g(3)) {} }

int main() { w1(); w2(); }
