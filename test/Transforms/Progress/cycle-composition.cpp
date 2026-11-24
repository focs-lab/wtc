// What a function's paths guarantee is composed once and remembered, but
// not while a function of its call cycle is still being composed: g reaches
// f's retry through the cycle, and is only lock-free whether root0 or g is
// composed first (-DROOT_LAST puts root0 after g).
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
// RUN: %clangxx -std=c++17 -I%shim -DROOT_LAST -fclangir -emit-cir %s -o %t.last.cir
// RUN: not wtc-opt %t.last.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: error: wtc: g(int) is declared wait-free but is only lock-free
// CHECK: note: the weakest step: retries a compare-and-swap that fails only when another thread changed the location: lock-free

#include "progress.hpp"
#include <atomic>

std::atomic<int> c{0};
void f(int n);

#ifndef ROOT_LAST
WTC_LOCK_FREE void root0() { f(3); }
#endif

WTC_WAIT_FREE void g(int n) {
    if (n) f(n - 1);
}

void f(int n) {
    g(n);
    int e = c.load(std::memory_order_relaxed);
    while (!c.compare_exchange_weak(e, e + 1, std::memory_order_relaxed)) {}
}

#ifdef ROOT_LAST
WTC_LOCK_FREE void root0() { f(3); }
#endif
int main() { root0(); g(2); }
