// A thread that raises its own priority through a helper is at real-time
// priority from its start routine on: the requirement is the worker's, and
// its mutex is reported.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: warning: wtc: worker(void*) is a thread at real-time priority (SCHED_FIFO, pthread_setschedparam), so it must be wait-free but for waits for data, but it can block
// CHECK: note: which waits in the threading library (pthread_mutex_lock)

#include <pthread.h>
#include <sched.h>

pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
int shared;

static void make_realtime() {
    sched_param p{};
    p.sched_priority = 50;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
}

void *worker(void *) {
    make_realtime();
    for (int i = 0; i < 100; ++i) {
        pthread_mutex_lock(&m);
        ++shared;
        pthread_mutex_unlock(&m);
    }
    return nullptr;
}

int main() {
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
    pthread_join(t, nullptr);
}
