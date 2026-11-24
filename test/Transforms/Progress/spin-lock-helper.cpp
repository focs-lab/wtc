// A spin lock whose compare-and-swap sits in a helper of the program's
// (try_lock): the loop that calls it waits for the holder, which the check
// cannot tell from a retry, so it is counted as a spin lock, under any
// promise.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: error: wtc: take() is declared lock-free but can block
// CHECK-DAG: note: which spins until another thread changes what it reads (a compare-and-swap inside a helper, which the check cannot tell from a spin lock's test-and-set): it waits for another thread
// CHECK-DAG: error: wtc: take_wf() is declared wait-free within 1000 steps but can block

#include "progress.hpp"
#include <atomic>

struct spin {
    std::atomic<bool> f{false};
    bool try_lock() {
        bool e = false;
        return f.compare_exchange_strong(e, true, std::memory_order_acquire);
    }
    void lock() { while (!try_lock()) {} }
    void unlock() { f.store(false, std::memory_order_release); }
};

spin s;
int shared;

WTC_LOCK_FREE void take() {
    s.lock();
    ++shared;
    s.unlock();
}

WTC_WAIT_FREE_WITHIN(1000) void take_wf() {
    s.lock();
    ++shared;
    s.unlock();
}

int main() { take(); take_wf(); }
