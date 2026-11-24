// A thread at real-time priority never yields to a thread of lower
// priority on its processor, so its paths must be wait-free, waits for
// data apart, and a wait for data must not spin. Real-time threads are
// read from the program: pthread_attr_setschedpolicy with explicit
// scheduling on the attributes pthread_create is handed, and
// pthread_setschedparam on a thread it started. A consumer that waits on
// a queue that spins (the default) is refused for the spin; one that waits
// on a queue whose policy parks passes, the runtime having measured its
// tick rate when the queue was built; one that takes a mutex can block.
// The requirement is unstated, so the verdicts are warnings.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: remark: wtc: queue @spinning: spsc (producers: 1, consumers: 1)
// CHECK-DAG: warning: wtc: rt_spin(void*) is a thread at real-time priority (SCHED_FIFO, pthread_attr_setschedpolicy), so it must be wait-free but for waits for data, but it can block
// CHECK-DAG: note: which waits for a push by spinning (its queue's wait policy is spin): a real-time thread never yields to a producer of lower priority on its processor
// CHECK-DAG: remark: wtc: rt_park(void*) is a thread at real-time priority (SCHED_FIFO, pthread_attr_setschedpolicy), so it must be wait-free but for waits for data: it is
// CHECK-DAG: warning: wtc: rt_lock(void*) is a thread at real-time priority (SCHED_RR, pthread_setschedparam), so it must be wait-free but for waits for data, but it can block
// CHECK-DAG: note: which waits in the threading library (pthread_mutex_lock)

#include "queue.hpp"

#include <pthread.h>
#include <sched.h>

wtc::queue<int> spinning;
wtc::queue<int> parking(wtc::topology::generic, wtc::wait_policy::park);
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
int shared = 0;

void *rt_spin(void *) {
    int v = 0;
    spinning.pop_wait(v);
    return nullptr;
}

void *rt_park(void *) {
    int v = 0;
    parking.pop_wait(v);
    return nullptr;
}

void *rt_lock(void *) {
    pthread_mutex_lock(&m);
    ++shared;
    pthread_mutex_unlock(&m);
    return nullptr;
}

void *producer(void *) {
    spinning.push(1);
    parking.push(1);
    return nullptr;
}

int main() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_t a, b, c, p;
    pthread_create(&a, &attr, rt_spin, nullptr);
    pthread_create(&b, &attr, rt_park, nullptr);
    pthread_create(&c, nullptr, rt_lock, nullptr);
    sched_param sp{};
    sp.sched_priority = 10;
    pthread_setschedparam(c, SCHED_RR, &sp);
    pthread_create(&p, nullptr, producer, nullptr);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    pthread_join(c, nullptr);
    pthread_join(p, nullptr);
}
