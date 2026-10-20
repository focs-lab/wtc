// A consumer that keeps a copy of what it pops is not a borrower, so the
// compiler leaves its queue plain spsc and the program is correct. Forcing
// inline storage on it anyway is the negative control: under WTC_CHECK the
// guard must catch the kept element at the next pop.
//
// RUN: %wtc_cc %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=PASSES
// RUN: %t.exe | FileCheck %s
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s --check-prefix=PASSES %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,CHECK %}
// RUN: %wtc_cc -DWTC_CHECK --force-inline q %s -o %t.forced.exe
// RUN: not --crash %t.forced.exe 2>&1 | FileCheck %s --check-prefix=CAUGHT
//
// PASSES: queue @q: not borrowed: the popped shared_ptr is passed to std::shared_ptr<record>::operator=
// PASSES: tag rewritten to spsc{{$}}
// TSAN-NOT: ThreadSanitizer
// CHECK: tag spsc last 999
// CAUGHT: wtc: borrowed element still referenced at the next try_pop on queue

#include "queue.hpp"
#include "thread.hpp"

#include <cstdio>
#include <memory>

struct record {
    long seq;
};

wtc::queue<record> q;
std::shared_ptr<record> last;

void produce() {
    for (long i = 0; i < 1000; ++i) q.push(record{i});
}

void consume() {
    for (int got = 0; got < 1000;) {
        auto p = q.try_pop();
        if (!p) continue;
        last = p;
        ++got;
    }
}

int main() {
    std::thread a = wtc::spawn(produce);
    std::thread b = wtc::spawn(consume);
    a.join();
    b.join();
    std::printf("tag %s last %ld\n", q.tag() == wtc::topology::spsc ? "spsc" : "other", last->seq);
    return 0;
}
