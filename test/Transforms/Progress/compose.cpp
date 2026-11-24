// What a declared function's paths guarantee, composed and reported: the
// level, the steps under the thread counts the analysis proved, and the
// effects beside them, whether or not the promise holds. The consumer of
// an spsc queue takes at most its 67 steps and its slip's 96 pauses, and
// passes; four of its pops in a counted loop take four times that. The
// consumer of an mpsc queue that three producers, started by a loop of
// three, feed is wait-free with a worst case instantiated at np = 3, its
// hole looks, and one retire whose scan reads the n hazard records, which
// keeps n; it fails on what it frees and may allocate. A retry on a local
// atomic never fails, so its steps are counted (each of the library's
// atomic accesses, summed over the cases of the memory order it switches
// on); a retry on a shared one is lock-free.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: remark: wtc: queue @in: mpsc (producers: many, consumers: 1)
// CHECK-DAG: remark: wtc: take_one(int&) is declared lock-free: nothing it reaches blocks
// CHECK-DAG: remark: wtc: take_one(int&): wait-free, at most 163 steps (67 + 96 slip pauses)
// CHECK-DAG: remark: wtc: take_four(int*): wait-free, at most 652 steps (268 + 384 slip pauses)
// CHECK-DAG: error: wtc: take_in(int&) is declared lock-free but can block
// CHECK-DAG: remark: wtc: take_in(int&): wait-free, at most 49·n² + 70·n + 924558 + 68 slip pauses steps; allocates (may); on first use, a per-thread record; with np = 3 for @in
// CHECK-DAG: remark: wtc: local_retry(): wait-free, at most {{[0-9]+}} steps
// CHECK-DAG: remark: wtc: shared_retry(): lock-free
// CHECK-DAG: note: the weakest step: retries a compare-and-swap that fails only when another thread changed the location: lock-free

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <atomic>

wtc::queue<int> one;
wtc::queue<int> in;
std::atomic<long> shared{0};

WTC_LOCK_FREE bool take_one(int &v) { return one.try_pop(v); }

WTC_LOCK_FREE void take_four(int *v) {
    for (int i = 0; i < 4; ++i) one.try_pop(v[i]);
}

WTC_LOCK_FREE bool take_in(int &v) { return in.try_pop(v); }

WTC_LOCK_FREE long local_retry() {
    std::atomic<long> mine{0};
    long old = mine.load(std::memory_order_relaxed);
    while (!mine.compare_exchange_strong(old, old + 1, std::memory_order_relaxed)) {}
    return mine.load(std::memory_order_relaxed);
}

WTC_LOCK_FREE void shared_retry() {
    long old = shared.load(std::memory_order_relaxed);
    while (!shared.compare_exchange_weak(old, old + 1, std::memory_order_relaxed)) {}
}

void put_one() { one.push(1); }
void put_in() { in.push(1); }

void consumer() {
    int v = 0, w[4];
    take_one(v);
    take_four(w);
    take_in(v);
    local_retry();
    shared_retry();
}

int main() {
    std::thread p = wtc::spawn(put_one);
    std::thread ps[3];
    for (int i = 0; i < 3; ++i) ps[i] = wtc::spawn(put_in);
    std::thread c = wtc::spawn(consumer);
    p.join();
    for (auto &t : ps) t.join();
    c.join();
}
