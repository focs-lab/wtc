// A first-use effect blocks the same way in the program's own code and in
// the runtime's. A function-local static of the program's takes a
// static-initialisation guard and fails its promise. The spsc pop takes
// none: the runtime measures the tick rate its slip reads when the queue
// is built, so the consumer passes. A thread's first operation on the
// wait-free ring of indices takes a per-thread record (a hazard record and
// a thread slot), whose pthread_setspecific may call the allocator, and a
// consumer declared wait-free fails on it; the ring below stands for a
// runtime that announces wcq, and the program states its tag.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: error: wtc: own_static() is declared lock-free but can block
// CHECK-DAG: note: which takes a static-initialisation lock (__cxa_guard_acquire)
// CHECK-DAG: remark: wtc: take(int&) is declared wait-free: it is
// CHECK-DAG: error: wtc: take_wcq(int&) is declared wait-free but can block
// CHECK-DAG: note: which takes a per-thread record the first time its thread runs it (hazard::tied_record), whose pthread_setspecific calls the allocator unless the key is among the C library's first 32
// CHECK-DAG: remark: wtc: take_wcq(int&): wait-free, finite, not instantiated (wCQ's steps are finite (SPAA'22) and its paper gives no closed form; {{.*}}); may enter the kernel (futex wake); on first use, a per-thread record
// CHECK-DAG: remark: wtc: not specialised for a requirement: the topology tag is the program's own; take_wcq(int&), declared wait-free: wcq: its bounded pop transfer takes, on first use, a per-thread record

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"
#include <chrono>

namespace offering {

template <class T>
struct ring {
    [[clang::annotate("wtc_queue_impl_wcq")]] static void offers_wcq(int, unsigned) {}
    [[clang::annotate("wtc_queue_construction")]] explicit ring(wtc::topology t = wtc::topology::generic) : tag(t) {
        offers_wcq(16, 128);
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

wtc::queue<int> q;
offering::ring<int> wring(static_cast<wtc::topology>(16));

static long calibrate() { return std::chrono::steady_clock::now().time_since_epoch().count() & 1; }

WTC_LOCK_FREE long own_static() {
    static const long rate = calibrate();
    return rate;
}

WTC_WAIT_FREE bool take(int &x) { return q.try_pop(x); }
WTC_WAIT_FREE bool take_wcq(int &x) { return wring.try_pop(x); }

void producer() {
    for (int i = 0; i < 1000; ++i) q.push(i);
    wring.try_push(1);
}

void consumer() {
    int x;
    while (!take(x)) {}
    take_wcq(x);
}

int main() {
    own_static();
    std::thread p = wtc::spawn(producer), c = wtc::spawn(consumer);
    p.join();
    c.join();
}
