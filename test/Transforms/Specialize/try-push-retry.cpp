// A loop that retries a bounded queue's try_push until it is taken waits
// for room, which another thread's pop makes: a dual wait, whatever the
// ring; on a real-time thread it spins. No ring meets the wait-free
// requirement on the push here (scq is lock-free, and wcq takes a
// per-thread record on first use), so the ring stays, and the push's own
// wait for the pop that last held its cell is reported beside the dual
// wait.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: remark: wtc: no implementation meets a requirement: put_all(int), declared wait-free: generic: its bounded try_push is blocking (starvation-free); scq: its bounded try_push is lock-free; wcq: its bounded try_push takes, on first use, a per-thread record
// CHECK-DAG: error: wtc: put_all(int) is declared wait-free but can block
// CHECK-DAG: note: which waits for an element of a queue, or for room in one: a dual wait, for data, not for an operation in progress
// CHECK-DAG: note: which waits for an element of a queue, or for room in one: a dual wait, for data, not for an operation in progress; it spins, and a real-time thread never yields to a producer of lower priority on its processor

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"
#include <pthread.h>
#include <sched.h>

namespace offering {
template <class T>
struct ring {
    [[clang::annotate("wtc_queue_impl_scq")]] static void offers_scq(int) {}
    [[clang::annotate("wtc_queue_impl_wcq")]] static void offers_wcq(int) {}
    [[clang::annotate("wtc_queue_construction")]] explicit ring(wtc::topology t = wtc::topology::generic) : tag(t) {
        offers_scq(15);
        offers_wcq(16);
    }
    [[clang::annotate("wtc_queue_try_push"), clang::annotate("wtc_queue_bounded")]] bool try_push(const T &v) {
        last = v;
        return true;
    }
    [[clang::annotate("wtc_queue_try_pop_into"), clang::annotate("wtc_queue_bounded")]] bool try_pop(T &v) {
        v = last;
        return true;
    }
    wtc::topology tag;
    T last{};
};
} // namespace offering

offering::ring<int> r;

WTC_WAIT_FREE void put_all(int v) {
    while (!r.try_push(v)) {}
}

void *rt_producer(void *) {
    for (int i = 0; i < 100; ++i)
        while (!r.try_push(i)) {}
    return nullptr;
}

void consumer() {
    int v = 0;
    r.try_pop(v);
}

int main() {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    pthread_t p;
    pthread_create(&p, &a, rt_producer, nullptr);
    put_all(1);
    std::thread c = wtc::spawn(consumer), d = wtc::spawn(consumer);
    c.join();
    d.join();
    pthread_join(p, nullptr);
}
