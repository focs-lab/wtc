// A real-time thread that polls a queue in a loop it never leaves: no exit
// is no finite path, and the polling spins while a producer of lower
// priority waits for the processor.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: warning: wtc: rt_poll(void*) is a thread at real-time priority (SCHED_FIFO, pthread_attr_setschedpolicy), so it must be wait-free but for waits for data, but it can block
// CHECK-DAG: note: which polls a queue in a loop it never leaves: a wait for data that never ends

#include "queue.hpp"
#include <pthread.h>
#include <sched.h>

wtc::queue<int> q;
int sum;

void *rt_poll(void *) {
    for (;;) {
        int x;
        if (q.try_pop(x)) sum += x;
    }
    return nullptr;
}

void *producer(void *) {
    for (int i = 0; i < 1000; ++i) q.push(i);
    return nullptr;
}

int main() {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    pthread_t c, p;
    pthread_create(&c, &a, rt_poll, nullptr);
    pthread_create(&p, nullptr, producer, nullptr);
    pthread_join(p, nullptr);
    pthread_join(c, nullptr);
}
