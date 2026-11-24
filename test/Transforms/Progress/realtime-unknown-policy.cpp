// A scheduling policy that is not a constant here, read from a
// configuration and set on main's own thread, is not taken for a
// real-time requirement: it would hold everything main reaches, and every
// thread it starts, to wait-freedom. It is reported as not checked; a
// program that means it states the requirement on the thread's entry.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: warning: wtc: sets a scheduling policy not known here, which is not taken for a real-time requirement (state one with WTC_WAIT_FREE on the thread's entry), so what it runs is not checked
// CHECK-NOT: is a thread at real-time priority

#include <pthread.h>
#include <sched.h>
#include <cstdlib>

pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
int shared;

void *worker(void *) {
    pthread_mutex_lock(&m);
    ++shared;
    pthread_mutex_unlock(&m);
    return nullptr;
}

int main(int argc, char **argv) {
    const int policy = argc > 1 ? std::atoi(argv[1]) : SCHED_OTHER;
    sched_param p{};
    pthread_setschedparam(pthread_self(), policy, &p);
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
    pthread_join(t, nullptr);
}
