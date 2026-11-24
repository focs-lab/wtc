// A bound that does not fit in 64 bits is no number a promise can be held
// to: three nested loops of 2^22 trips around an atomic store saturate to a
// finite bound that is not instantiated, and fail a promise of one step.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: error: wtc: huge() is declared wait-free within 1 steps but has a bound that is not a number here: finite, not instantiated (too large to count in 64 bits)

#include "progress.hpp"
#include <atomic>

std::atomic<int> x{0};

WTC_WAIT_FREE_WITHIN(1) void huge() {
    for (int i = 0; i < 4194304; ++i)
        for (int j = 0; j < 4194304; ++j)
            for (int k = 0; k < 4194304; ++k) x.store(k, std::memory_order_relaxed);
}

int main() { huge(); }
