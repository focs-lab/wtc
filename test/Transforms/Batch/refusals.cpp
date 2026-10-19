// Loops that must not be batched, each with the reason that stands in the
// way, and a loop outside a WTC_BATCH function, about which nothing is
// said: batching delays elements, so only the program can ask for it.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-pushes -o %t.out.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir

#include "mutex.hpp"
#include "progress.hpp"
#include "queue.hpp"

#include <atomic>

wtc::queue<int> q, other;
wtc::mutex m;
std::atomic<bool> stop{false};
void may_throw(int);
void (*hook)(int);
extern "C" int pthread_yield_now();  // stands for any call into the threading library

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: the loop acts on queues or mutexes other than through one push
WTC_BATCH void pops(int n) { for (int i = 0; i < n; ++i) { q.push(i); other.try_pop(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: the push is not made on every iteration
WTC_BATCH void conditional(int n) { for (int i = 0; i < n; ++i) if (i % 2) q.push(i); }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: a continue skips the push on some iterations
WTC_BATCH void skips(int n) { for (int i = 0; i < n; ++i) { if (i == 3) continue; q.push(i); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: control can leave the loop other than at its end or a break
WTC_BATCH void returns(int n) { for (int i = 0; i < n; ++i) { q.push(i); if (i == 7) return; } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it calls std::atomic<bool>::load(std::memory_order) const, where {{.*}}it synchronises
WTC_BATCH void spins(int n) { for (int i = 0; i < n && !stop.load(); ++i) q.push(i); }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it calls may_throw(int), which may throw
WTC_BATCH void throws(int n) { for (int i = 0; i < n; ++i) { q.push(i); may_throw(i); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it makes an indirect call
WTC_BATCH void hooked(int n) { for (int i = 0; i < n; ++i) { q.push(i); hook(i); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: the loop acts on queues or mutexes other than through one push
WTC_BATCH void locks(int n) { for (int i = 0; i < n; ++i) { m.lock(); q.push(i); m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it calls into the threading library (pthread_yield_now)
WTC_BATCH void yields(int n) { for (int i = 0; i < n; ++i) { q.push(i); pthread_yield_now(); } }

void unmarked(int n) { for (int i = 0; i < n; ++i) q.push(i); }

// CHECK-NOT: pushes in this loop batched
// IR-NOT: wtc.queue_push_deferred
