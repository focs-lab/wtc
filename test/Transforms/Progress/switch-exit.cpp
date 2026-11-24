// A loop that leaves by a return inside a switch on an atomic: the
// switch's condition is one of its exits, and it waits for another thread
// to set the state.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: error: wtc: await_ready() is declared lock-free but can block
// CHECK: note: which spins until another thread changes what it reads (an atomic or volatile load): it waits for another thread

#include "progress.hpp"
#include <atomic>

std::atomic<int> state{0};

WTC_LOCK_FREE int await_ready() {
    for (;;) {
        switch (state.load(std::memory_order_acquire)) {
        case 1: return 1;
        case 2: return 2;
        default: break;
        }
    }
}

int main() { return await_ready(); }
