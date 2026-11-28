// A requirement refuses batching for the mutexes a requiring path may
// take, told apart by what they are: a real-time thread takes a Log
// object's member mutex, so a loop over another Log's member mutex is not
// batched (members of one record and field are not told apart by object),
// while a loop over a global mutex is: a mutex is held by value, so a
// global and a member are different objects. Before, any mutex that was
// not a global by name could have been the real-time thread's.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks=cost=off -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: progress-members.cpp:29:{{.*}} remark: wtc: critical sections in this loop batched
// CHECK-DAG: progress-members.cpp:37:{{.*}} remark: wtc: critical sections in this loop not batched: another thread that takes this mutex is reached from rt_entry(void*), a thread at real-time priority (SCHED_FIFO, pthread_attr_setschedpolicy): a batch makes it wait for up to WTC_LOCK_BATCH_K sections

#include "progress.hpp"
#include "mutex.hpp"
#include <pthread.h>
#include <sched.h>

struct Log {
    wtc::mutex m;
    long lines = 0;
};
Log rt_log, other_log;
wtc::mutex stats_m;
long hits;

WTC_BATCH void count_hits() {
    for (int i = 0; i < 100000; ++i) {
        stats_m.lock();
        ++hits;
        stats_m.unlock();
    }
}

WTC_BATCH void log_lines() {
    for (int i = 0; i < 100000; ++i) {
        other_log.m.lock();
        ++other_log.lines;
        other_log.m.unlock();
    }
}

void *rt_entry(void *) {
    rt_log.m.lock();
    ++rt_log.lines;
    rt_log.m.unlock();
    return nullptr;
}

void *plain_entry(void *) {
    count_hits();
    log_lines();
    return nullptr;
}

int main() {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    pthread_t r, p;
    pthread_create(&r, &a, rt_entry, nullptr);
    pthread_create(&p, nullptr, plain_entry, nullptr);
    pthread_join(r, nullptr);
    pthread_join(p, nullptr);
}
