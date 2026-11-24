// Each promise, kept and broken. Wait-free: an atomic increment keeps it,
// a retry on a shared compare-and-swap is only lock-free, and an spsc
// consumer's transfer keeps it. Wait-free within k steps: a counted loop
// of eight atomic increments within 100 steps, not within 4, and a loop
// over the thread's own data with an atomic access in it has no number to
// compare. Progress-critical: an atomic increment is, and so is the spsc
// consumer, whose worst case is a constant and which has no first-use
// effect; the loop over own data has no polynomial worst case.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: remark: wtc: wf_count() is declared wait-free: it is
// CHECK-DAG: error: wtc: wf_retry() is declared wait-free but is only lock-free
// CHECK-DAG: note: the weakest step: retries a compare-and-swap that fails only when another thread changed the location: lock-free
// CHECK-DAG: remark: wtc: wf_take(int&) is declared wait-free: it is
// CHECK-DAG: remark: wtc: wf_take(int&): wait-free, at most 163 steps (67 + 96 slip pauses)
// CHECK-DAG: remark: wtc: within_eight() is declared wait-free within 100 steps: it is
// CHECK-DAG: error: wtc: tight_eight() is declared wait-free within 4 steps but may take {{[0-9]+}} steps
// CHECK-DAG: error: wtc: within_scan(int const*) is declared wait-free within 100 steps but has a bound that is not a number here: finite, not instantiated (a loop over the thread's own data)
// CHECK-DAG: remark: wtc: critical_count() is declared progress-critical: it is
// CHECK-DAG: remark: wtc: critical_take(int&) is declared progress-critical: it is
// CHECK-DAG: error: wtc: critical_scan(int const*) is declared progress-critical but has a worst case finite, not instantiated (a loop over the thread's own data)

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <atomic>

wtc::queue<int> q;
std::atomic<long> counter{0};

WTC_WAIT_FREE void wf_count() { counter.fetch_add(1, std::memory_order_relaxed); }
WTC_WAIT_FREE bool wf_take(int &v) { return q.try_pop(v); }
WTC_WAIT_FREE void wf_retry() {
    long old = counter.load(std::memory_order_relaxed);
    while (!counter.compare_exchange_weak(old, old + 1, std::memory_order_relaxed)) {}
}
WTC_WAIT_FREE_WITHIN(100) void within_eight() {
    for (int i = 0; i < 8; ++i) counter.fetch_add(1, std::memory_order_relaxed);
}
WTC_WAIT_FREE_WITHIN(4) void tight_eight() {
    for (int i = 0; i < 8; ++i) counter.fetch_add(1, std::memory_order_relaxed);
}
WTC_WAIT_FREE_WITHIN(100) int within_scan(const int *a) {
    int n = 0;
    while (a[n] != 0) {
        counter.fetch_add(1, std::memory_order_relaxed);
        ++n;
    }
    return n;
}
WTC_PROGRESS_CRITICAL void critical_count() { counter.fetch_add(1, std::memory_order_relaxed); }
WTC_PROGRESS_CRITICAL bool critical_take(int &v) { return q.try_pop(v); }
WTC_PROGRESS_CRITICAL int critical_scan(const int *a) {
    int n = 0;
    while (a[n] != 0) {
        counter.fetch_add(1, std::memory_order_relaxed);
        ++n;
    }
    return n;
}

void producer() { q.push(1); }

void consumer() {
    int v = 0, a[2] = {1, 0};
    wf_count();
    wf_take(v);
    wf_retry();
    within_eight();
    tight_eight();
    within_scan(a);
    critical_count();
    critical_take(v);
    critical_scan(a);
}

int main() {
    std::thread p = wtc::spawn(producer);
    std::thread c = wtc::spawn(consumer);
    p.join();
    c.join();
}
