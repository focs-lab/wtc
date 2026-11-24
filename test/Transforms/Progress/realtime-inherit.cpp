// A thread started from a real-time thread with the default attributes
// inherits its policy: it is checked as a real-time thread of its own.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: warning: wtc: helper(void*) is a thread at real-time priority (inherited from rt_main(void*), SCHED_FIFO, pthread_attr_setschedpolicy), so it must be wait-free but for waits for data, but it can block
// CHECK-DAG: note: which waits in the threading library (pthread_mutex_lock)

#include <pthread.h>
#include <sched.h>

pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
int shared;

void *helper(void *) {
    pthread_mutex_lock(&m);
    ++shared;
    pthread_mutex_unlock(&m);
    return nullptr;
}

void *rt_main(void *) {
    pthread_t h;
    pthread_create(&h, nullptr, helper, nullptr);  // inherits SCHED_FIFO
    return nullptr;
}

int main() {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    pthread_t t;
    pthread_create(&t, &a, rt_main, nullptr);
    pthread_join(t, nullptr);
}
