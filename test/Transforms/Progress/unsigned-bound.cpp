// A counted loop whose constant bound is an unsigned number of 2^31 or
// more runs that many times: read with the comparison's signedness, not as
// a negative number.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: error: wtc: many() is declared wait-free within 10 steps but may take 9000000000 steps (9000000000)

#include "progress.hpp"
#include <atomic>

std::atomic<unsigned> x{0};

WTC_WAIT_FREE_WITHIN(10) void many() {
    for (unsigned i = 0; i < 3000000000u; ++i) x.store(i, std::memory_order_relaxed);
}

int main() { many(); }
