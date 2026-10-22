// Loops whose critical sections must not be held as one, each with the
// reason that stands in the way. The rules are stricter than for batched
// pushes: code between the sections runs with the mutex held, so nothing in
// the loop may wait for another thread, which may need the mutex to go on.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks -o %t.out.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir

#include "mutex.hpp"
#include "progress.hpp"

#include <atomic>

wtc::mutex m, other;
std::atomic<bool> stop{false};
long total;
extern "C" long read_input(int fd) noexcept;   // stands for any input or output
void may_throw(int);
void bump() { m.lock(); ++total; m.unlock(); }

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls read_input, which may wait for another thread
WTC_BATCH void reads(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; m.unlock(); read_input(0); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: a continue skips the critical section on some iterations
WTC_BATCH void skips(int n) { for (int i = 0; i < n; ++i) { if (i == 3) continue; m.lock(); ++total; m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: a break between the lock and the unlock leaves with the mutex held
WTC_BATCH void leaves(int n) { for (int i = 0; i < n; ++i) { m.lock(); if (i == 3) break; ++total; m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: the loop acts on queues or mutexes other than through one lock and one unlock
WTC_BATCH void nests(int n) { for (int i = 0; i < n; ++i) { m.lock(); other.lock(); ++total; other.unlock(); m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: the critical section is not entered on every iteration
WTC_BATCH void sometimes(int n) { for (int i = 0; i < n; ++i) if (i % 2) { m.lock(); ++total; m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: the loop lets go of a different mutex than it takes
WTC_BATCH void swaps(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; other.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls std::atomic<bool>::load(std::memory_order) const, where {{.*}}it synchronises
WTC_BATCH void spins(int n) { for (int i = 0; i < n && !stop.load(); ++i) { m.lock(); ++total; m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls may_throw(int), which may throw
WTC_BATCH void throws(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; m.unlock(); may_throw(i); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls bump(), where it acts on a queue or a mutex
WTC_BATCH void calls(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; m.unlock(); bump(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: the loop takes the mutex and does not let it go
WTC_BATCH void holds(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: the unlock does not follow the lock in the same block
WTC_BATCH void reversed(int n) { for (int i = 0; i < n; ++i) { m.unlock(); ++total; m.lock(); } }
// A break outside the critical section is no obstacle: the mutex is held
// there now, and let go after the loop.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched
WTC_BATCH void stops(int n) { for (int i = 0; i < n; ++i) { if (i == 3) break; m.lock(); ++total; m.unlock(); } }
// Without the mark nothing is said or done.
// CHECK-NOT: [[@LINE+1]]:{{.*}} remark
void unmarked(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; m.unlock(); } }

// IR-LABEL: cir.func {{.*}}@_Z5readsi(
// IR-NOT: lock_batch
// IR: cir.return
