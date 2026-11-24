// Loops are judged by what their exit depends on. A retry on a
// compare-and-swap is lock-free and passes, and so do a counted loop and
// one over the thread's own data. A spin on a flag another thread sets
// waits for that thread, and so does a spin lock built from a
// compare-and-swap of a constant or from an exchange; a loop that waits
// for an element of a queue waits for data, which a function declared
// lock-free does not do either.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: remark: wtc: cas_retry() is declared lock-free: nothing it reaches blocks
// CHECK-DAG: remark: wtc: counted(int*) is declared lock-free: nothing it reaches blocks
// CHECK-DAG: remark: wtc: own_data(int const*) is declared lock-free: nothing it reaches blocks
// CHECK-DAG: error: wtc: flag_spin() is declared lock-free but can block
// CHECK-DAG: note: which spins until another thread changes what it reads (an atomic or volatile load): it waits for another thread
// CHECK-DAG: error: wtc: volatile_spin() is declared lock-free but can block
// CHECK-DAG: error: wtc: break_spin() is declared lock-free but can block
// CHECK-DAG: error: wtc: tas_lock() is declared lock-free but can block
// CHECK-DAG: note: which spins until another thread changes what it reads (a compare-and-swap of an expected value it sets itself, a spin lock): it waits for another thread
// CHECK-DAG: error: wtc: xchg_lock() is declared lock-free but can block
// CHECK-DAG: note: which spins until another thread changes what it reads (an exchange or a fetch-and-op): it waits for another thread
// CHECK-DAG: error: wtc: wait_for_item(int&) is declared lock-free but can block
// CHECK-DAG: note: which waits for an element of a queue, or for room in one: a dual wait, for data, not for an operation in progress

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <atomic>

std::atomic<long> counter{0};
std::atomic<bool> ready{false};
std::atomic<bool> lock_word{false};
volatile int vflag = 0;
wtc::queue<int> items;

WTC_LOCK_FREE void cas_retry() {
    long old = counter.load(std::memory_order_relaxed);
    while (!counter.compare_exchange_weak(old, old + 1, std::memory_order_relaxed)) {}
}

WTC_LOCK_FREE void counted(int *a) {
    for (int i = 0; i < 16; ++i) a[i] = i;
}

WTC_LOCK_FREE int own_data(const int *a) {
    int n = 0;
    while (a[n] != 0) ++n;
    return n;
}

WTC_LOCK_FREE void flag_spin() {
    while (!ready.load(std::memory_order_acquire)) {}
}

WTC_LOCK_FREE void volatile_spin() {
    while (vflag == 0) {}
}

WTC_LOCK_FREE void break_spin() {
    for (;;) {
        if (ready.load(std::memory_order_acquire)) break;
    }
}

WTC_LOCK_FREE void tas_lock() {
    bool expected = false;
    while (!lock_word.compare_exchange_weak(expected, true, std::memory_order_acquire)) expected = false;
    lock_word.store(false, std::memory_order_release);
}

WTC_LOCK_FREE void xchg_lock() {
    while (lock_word.exchange(true, std::memory_order_acquire)) {}
    lock_word.store(false, std::memory_order_release);
}

WTC_LOCK_FREE void wait_for_item(int &v) {
    while (!items.try_pop(v)) {}
}

void producer() {
    items.push(1);
    ready.store(true);
    vflag = 1;
}

void consumer() {
    int a[17] = {1, 2, 0};
    int v = 0;
    cas_retry();
    counted(a);
    own_data(a);
    flag_spin();
    volatile_spin();
    break_spin();
    tas_lock();
    xchg_lock();
    wait_for_item(v);
}

int main() {
    std::thread p = wtc::spawn(producer);
    std::thread c = wtc::spawn(consumer);
    p.join();
    c.join();
}
