// A test-and-test-and-set lock: the loop reads the word, continues while it
// is held, and tries its compare-and-swap only when it is free. Its exit
// depends on the compare-and-swap, but whether it tries one depends on the
// value it read: it waits for the holder, as a spin lock does, and is no
// retry.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: error: wtc: take() is declared lock-free but can block
// CHECK: note: which retries a compare-and-swap only when what it read says the location is free, and otherwise skips it: it waits on the value it read, as a spin lock does

#include "progress.hpp"
#include <atomic>

std::atomic<int> word{0};
int shared;

WTC_LOCK_FREE void take() {
    for (;;) {
        int e = word.load(std::memory_order_relaxed);
        if (e != 0) continue;  // held: wait for the holder
        if (word.compare_exchange_weak(e, 1, std::memory_order_acquire)) break;
    }
    ++shared;
    word.store(0, std::memory_order_release);
}

int main() { take(); }
