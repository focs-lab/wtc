// Batching delays each element by up to WTC_BATCH_K - 1 pushes: the
// latency a progress-critical consumer, or one on a real-time thread, must
// not pay. The pushes of a queue such a consumer pops are not batched;
// those of another queue are. Holding a mutex across sections makes every
// other thread that takes it wait longer: a loop's sections are not
// batched when a thread with a requirement takes the same mutex.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-pushes --wtc-batch-locks=cost=off -o /dev/null 2>&1 | FileCheck %s
//

#include "mutex.hpp"
#include "progress.hpp"
#include "queue.hpp"

struct Tick { long t; };
wtc::queue<Tick> ticks;
wtc::queue<int> logs;
wtc::mutex m;
long shared = 0;

WTC_PROGRESS_CRITICAL bool critical_take(Tick &t) { return ticks.try_pop(t); }

WTC_BATCH void feed_ticks(int n) {
    // CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: a consumer of this queue's type is reached from critical_take(Tick&), declared progress-critical: a batch delays each element by up to WTC_BATCH_K - 1 pushes, the latency that consumer must not pay
    for (int i = 0; i < n; ++i) ticks.push(Tick{i});
}

WTC_BATCH void feed_logs(int n) {
    // CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop batched
    for (int i = 0; i < n; ++i) logs.push(i);
}

WTC_BATCH void bump(int n) {
    // CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: another thread that takes this mutex is reached from counted_lock_free(), declared lock-free: a batch makes it wait for up to WTC_LOCK_BATCH_K sections
    for (int i = 0; i < n; ++i) {
        m.lock();
        ++shared;
        m.unlock();
    }
}

WTC_LOCK_FREE void counted_lock_free() {
    m.lock();
    ++shared;
    m.unlock();
}
