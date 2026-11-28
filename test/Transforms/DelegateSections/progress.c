// A delegated section makes its client wait for the server thread, which
// runs other clients' sections first, at the priority of the client that
// started it. A section reached from a path with a requirement of
// progress is not delegated: here one on a real-time thread. Neither is
// any section of a mutex that may use priority inheritance. A section of
// the same mutex on an ordinary thread still is.
//
// RUN: %clang -std=gnu11 -O1 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=inheriting -o /dev/null 2>&1 | FileCheck %s --check-prefix=PI

#include <pthread.h>
#include <sched.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t inheriting;
static long total, rt_total, pi_total;

// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section add #1 on lock delegated
long add(long x) {
    pthread_mutex_lock(&lock);
    total += x;
    long t = total;
    pthread_mutex_unlock(&lock);
    return t;
}

// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section rt_add #1 on lock not delegated: the section is reached from rt_worker through rt_add, a thread at real-time priority (SCHED_FIFO, pthread_setschedparam): a delegated section waits for the server thread, which runs other clients' sections first at the priority of the client that started it
void rt_add(long x) {
    pthread_mutex_lock(&lock);
    rt_total += x;
    pthread_mutex_unlock(&lock);
}

void *rt_worker(void *arg) {
    struct sched_param sp = {.sched_priority = 10};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    rt_add((long)arg);
    return 0;
}

// PI-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section pi_add #1 on inheriting not delegated: the mutex may use priority inheritance (pthread_mutexattr_setprotocol, then pthread_mutex_init): a waiter of high priority lifts the holder, and the server, at the priority of the client that started it, would invert it
void pi_add(long x) {
    pthread_mutex_lock(&inheriting);
    pi_total += x;
    pthread_mutex_unlock(&inheriting);
}

void init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&inheriting, &a);
}

int main(void) {
    pthread_t t;
    init();
    pthread_create(&t, 0, rt_worker, (void *)1);
    add(2);
    pi_add(3);
    pthread_join(t, 0);
    return 0;
}
