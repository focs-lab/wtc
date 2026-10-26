// One producer and four consumers: the analysis proves one producer and
// many consumers, and the queue becomes the one whose consumers claim the
// producer's cells. Every element arrives once, and each consumer sees its
// own in the order they were pushed; half of them are pushed in runs
// (batch_begin, push_deferred, batch_end), each published with one store.
// WTC_CHECK aborts if a second thread ever pushes.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
//
// CHECK: queue @q: spmc (producers: 1, consumers: many)
// CHECK: specialised: constructor tag rewritten to spmc
// TSAN-NOT: ThreadSanitizer
// OUT: tag 4 ok 400000

#include "queue.hpp"
#include "thread.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

wtc::queue<int> q;
constexpr int kTotal = 400000;
constexpr int kConsumers = 4;

std::atomic<int> got{0};
std::atomic<bool> ok{true};
std::atomic<unsigned char> seen[kTotal];

void consume() {
    int last = -1;
    while (got.load(std::memory_order_relaxed) < kTotal) {
        auto e = q.try_pop();
        if (!e) continue;
        const int v = *e;
        if (v <= last || v >= kTotal || seen[v].exchange(1)) ok = false;
        last = v;
        got.fetch_add(1, std::memory_order_relaxed);
    }
}

int main() {
    std::vector<std::thread> ts;
    for (int c = 0; c < kConsumers; ++c) ts.push_back(wtc::spawn(consume));
    for (int i = 0; i < kTotal / 2; ++i) q.push(i);
    wtc::queue<int>::batch b;
    for (int i = kTotal / 2; i < kTotal; i += 100) {
        q.batch_begin(b);
        for (int j = i; j < i + 100; ++j) q.push_deferred(b, j);
        q.batch_end(b);
    }
    for (auto& t : ts) t.join();
    std::printf("tag %d %s %d\n", static_cast<int>(q.tag()), ok ? "ok" : "LOST OR OUT OF ORDER",
                got.load());
}
