// One producer and a number of consumers read from the command line: with
// one consumer the queue is the inline SPSC ring, with three the ring whose
// consumers take turns, chosen when the queue is built. Every element
// arrives once either way. WTC_CHECK aborts if a second thread ever pops
// from the SPSC ring.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 1 | FileCheck %s --check-prefix=ONE
// RUN: %t.exe 3 | FileCheck %s --check-prefix=THREE
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 1 2>&1 | FileCheck %s --check-prefixes=TSAN,ONE %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 3 2>&1 | FileCheck %s --check-prefixes=TSAN,THREE %}
//
// CHECK: queue %q: spmc (producers: 1, consumers: many)
// CHECK: specialised at run time: spsc_inline when the loop at run_time_count.cpp:{{[0-9]+}}:{{[0-9]+}} (consumers) runs at most once, else spmc
// TSAN-NOT: ThreadSanitizer
// ONE: tag 2 ok 200000
// THREE: tag 4 ok 200000

#include "queue.hpp"
#include "thread.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

constexpr int kElements = 200000;
std::atomic<long> sum{0}, count{0};
int consumers = 1;

void produce(void *a) {
    auto *q = static_cast<wtc::queue<int> *>(a);
    for (int i = 1; i <= kElements; ++i) q->push(i);
    for (int c = 0; c < consumers; ++c) q->push(0);   // one end marker each
}

void consume(void *a) {
    auto *q = static_cast<wtc::queue<int> *>(a);
    for (;;) {
        auto p = q->try_pop();
        if (!p) { std::this_thread::yield(); continue; }
        if (*p == 0) return;
        sum.fetch_add(*p, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
    }
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    consumers = n;
    wtc::queue<int> q;
    std::vector<std::thread> pool;
    pool.push_back(wtc::spawn(produce, &q));
    for (int i = 0; i < n; i++) pool.push_back(wtc::spawn(consume, &q));
    for (auto &t : pool) t.join();
    const long want = long(kElements) * (kElements + 1) / 2;
    printf("tag %d ok %ld\n", (int)q.tag(), sum.load() == want ? count.load() : -1L);
}
