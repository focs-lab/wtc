// Four producers and one consumer: the analysis proves one consumer and
// many producers, and the queue becomes the list without locks. Every
// element arrives once, and each producer's in its order; one producer
// pushes in runs (batch_begin, push_deferred, batch_end), which splice in
// with one exchange each. WTC_CHECK aborts if a second thread ever pops.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
//
// CHECK: queue @q: mpsc (producers: many, consumers: 1)
// CHECK: specialised: constructor tag rewritten to mpsc
// TSAN-NOT: ThreadSanitizer
// OUT: tag 3 ok 400000

#include "queue.hpp"
#include "thread.hpp"

#include <cstdio>
#include <thread>
#include <vector>

struct item { int from; int seq; };

wtc::queue<item> q;
constexpr int kPer = 100000;

void plain(void* arg) {
    const int me = static_cast<int>(reinterpret_cast<long>(arg));
    for (int i = 0; i < kPer; ++i) q.push(item{me, i});
}

void runs(void* arg) {
    const int me = static_cast<int>(reinterpret_cast<long>(arg));
    wtc::queue<item>::batch b;
    for (int i = 0; i < kPer; i += 100) {
        q.batch_begin(b);
        for (int j = i; j < i + 100; ++j) q.push_deferred(b, item{me, j});
        q.batch_end(b);
    }
}

int main() {
    std::vector<std::thread> ts;
    for (long p = 0; p < 3; ++p) ts.push_back(wtc::spawn(plain, reinterpret_cast<void*>(p)));
    ts.push_back(wtc::spawn(runs, reinterpret_cast<void*>(3L)));
    int next[4] = {0, 0, 0, 0};
    bool ok = true;
    for (int got = 0; got < 4 * kPer;) {
        auto e = q.try_pop();
        if (!e) continue;
        if (e->seq != next[e->from]++) ok = false;
        ++got;
    }
    for (auto& t : ts) t.join();
    std::printf("tag %d %s %d\n", static_cast<int>(q.tag()), ok ? "ok" : "OUT OF ORDER",
                next[0] + next[1] + next[2] + next[3]);
}
