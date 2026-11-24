// A lifted wait on a real-time thread is allowed only where its waiter
// parks after its pause: the wait lifting builds it paced for a wait a
// requirement reaches, but a waiter built unpaced may spin and then yield
// before it parks (shim/lifted.hpp, lifted_wait_policy), which keeps a
// producer of lower priority off the real-time thread's processor. The
// waiter is found by the condition variable it lives in; the wait below is
// written as the lifting writes it. A construct elsewhere, unpaced, of a
// waiter the analysis cannot name may build this one too, so with it
// (-DUNNAMED) the paced waiter is not known to park either. (Every build
// still meets the step's own reading of the tick rate, which measures it
// through a clock the analysis cannot see into when no queue has measured
// it yet.)
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DPACED %s -o %t.paced.cir
// RUN: wtc-opt %t.paced.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s --check-prefix=PACED
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DPACED -DUNNAMED %s -o %t.unnamed.cir
// RUN: wtc-opt %t.unnamed.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: warning: wtc: rt_wait(void*) is a thread at real-time priority (SCHED_FIFO, pthread_attr_setschedpolicy), so it must be wait-free but for waits for data, but it can block
// CHECK: note: which waits in a lifted wait whose policy may spin, then yield, before it parks
//
// PACED: rt_wait(void*) is a thread at real-time priority
// PACED-NOT: lifted wait whose policy may spin

#include "lifted.hpp"

#include <atomic>
#include <condition_variable>
#include <pthread.h>
#include <sched.h>

#ifdef PACED
#define PACED_ARG true
#else
#define PACED_ARG false
#endif

struct Box {
    std::condition_variable cv;
    std::atomic<int> ready{0};
    using W = wtc::lifted_wait<Box>;
    Box() { W::construct(&cv, 1, 1, PACED_ARG); }
    ~Box() { W::destroy(&cv); }
    void post() {
        ready.store(1, std::memory_order_release);
        W::notify_one(&cv);
    }
};

Box box;

#ifdef UNNAMED
// Builds a waiter in storage the analysis cannot name.
void build_elsewhere(std::condition_variable *at) { Box::W::construct(at, 1, 1, false); }
#endif

// The wait, as the lifting writes it, in the real-time thread's own body.
void *rt_wait(void *) {
    Box::W::state st;
    Box::W::begin(&box.cv, &st);
    while (!box.ready.load(std::memory_order_acquire)) Box::W::step(&box.cv, &st);
    Box::W::end(&box.cv, &st);
    return nullptr;
}

int main() {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    pthread_t r;
    pthread_create(&r, &a, rt_wait, nullptr);
    box.post();
    pthread_join(r, nullptr);
}
