// Attributes are told apart by the field they are and by the array they
// are an element of. Two attribute members of one struct are two objects:
// explicit scheduling on cfg.rt does not make a thread started with
// cfg.plain stop inheriting, so the child a real-time thread starts with
// cfg.plain is real-time too and checked (-DMEMBERS). An element of an
// array at an index the analysis does not read is any element: a policy
// set on at[i] reaches the thread started with at[j] (-DELEMENTS).
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DMEMBERS %s -o %t.members.cir
// RUN: wtc-opt %t.members.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s --check-prefix=MEMBERS
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DELEMENTS %s -o %t.elements.cir
// RUN: wtc-opt %t.elements.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s --check-prefix=ELEMENTS
//
// MEMBERS: warning: wtc: child(void*) is a thread at real-time priority (inherited from rt_entry(void*), SCHED_FIFO, pthread_attr_setschedpolicy), so it must be wait-free but for waits for data, but it can block
// MEMBERS: note: which waits in the threading library (pthread_mutex_lock)
// ELEMENTS: warning: wtc: worker(void*) is a thread at real-time priority (SCHED_FIFO, pthread_attr_setschedpolicy), so it must be wait-free but for waits for data, but it can block
// ELEMENTS: note: which waits in the threading library (pthread_mutex_lock)

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

#if defined(MEMBERS)
struct Cfg {
    pthread_attr_t rt, plain;
} cfg;

void *child(void *) {
    pthread_mutex_lock(&m);
    ++shared;
    pthread_mutex_unlock(&m);
    return nullptr;
}

void *rt_entry(void *) {
    pthread_t c;
    pthread_create(&c, &cfg.plain, child, nullptr);
    pthread_join(c, nullptr);
    return nullptr;
}

int main() {
    pthread_attr_init(&cfg.rt);
    pthread_attr_init(&cfg.plain);
    pthread_attr_setinheritsched(&cfg.rt, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&cfg.rt, SCHED_FIFO);
    pthread_t r;
    pthread_create(&r, &cfg.rt, rt_entry, nullptr);
    pthread_join(r, nullptr);
}
#else
int main(int argc, char **) {
    pthread_attr_t at[2];
    const int i = argc > 1, j = argc > 2;
    pthread_attr_init(&at[i]);
    pthread_attr_setinheritsched(&at[i], PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at[i], SCHED_FIFO);
    pthread_t t;
    pthread_create(&t, &at[j], worker, nullptr);
    pthread_join(t, nullptr);
}
#endif
