// Real-time attributes set up by a helper: the attributes object is
// followed from the helper's parameter to what each call passes for it, so
// that a thread started with them is a real-time root. Both calls in the
// helper, or explicit scheduling in main and the policy in the helper
// (-DSPLIT). A helper that may be handed attributes from another
// translation unit (external linkage, -DEXTERN) is reported as not
// followed, rather than passing silently.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DSPLIT %s -o %t.split.cir
// RUN: wtc-opt %t.split.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DEXTERN %s -o %t.extern.cir
// RUN: wtc-opt %t.extern.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s --check-prefixes=CHECK,EXTERN
//
// EXTERN: warning: wtc: sets a real-time policy on attributes the analysis cannot follow to every thread they start, so what it runs is not checked
// CHECK: warning: wtc: worker(void*) is a thread at real-time priority (SCHED_FIFO, pthread_attr_setschedpolicy), so it must be wait-free but for waits for data, but it can block
// CHECK: note: which waits in the threading library (pthread_mutex_lock)

#include <pthread.h>
#include <sched.h>

pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
int shared;

#ifdef EXTERN
#define HELPER
#else
#define HELPER static
#endif

HELPER void realtime_attrs(pthread_attr_t *a) {
#ifndef SPLIT
    pthread_attr_setinheritsched(a, PTHREAD_EXPLICIT_SCHED);
#endif
    pthread_attr_setschedpolicy(a, SCHED_FIFO);
}

void *worker(void *) {
    pthread_mutex_lock(&m);
    ++shared;
    pthread_mutex_unlock(&m);
    return nullptr;
}

int main() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#ifdef SPLIT
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
#endif
    realtime_attrs(&attr);
    pthread_t t;
    pthread_create(&t, &attr, worker, nullptr);
    pthread_join(t, nullptr);
}
