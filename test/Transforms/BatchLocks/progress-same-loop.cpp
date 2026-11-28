// A loop run by a real-time thread and by an ordinary one: holding the
// mutex across its sections makes the other wait through them, so the loop
// the batch decides about counts as a taker of the mutex with a
// requirement.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: remark: wtc: critical sections in this loop not batched: a thread that runs this loop is reached from rt_entry(void*), a thread at real-time priority (SCHED_FIFO, pthread_attr_setschedpolicy): a batch makes it wait for up to WTC_LOCK_BATCH_K sections

#include "progress.hpp"
#include "mutex.hpp"
#include <pthread.h>
#include <sched.h>

wtc::mutex m;
long counter;

WTC_BATCH void work() {
    for (int i = 0; i < 100000; ++i) {
        m.lock();
        ++counter;
        m.unlock();
    }
}

void *rt_entry(void *) {
    work();
    return nullptr;
}

void *plain_entry(void *) {
    work();
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
