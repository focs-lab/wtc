// The negative controls for tsan_own_build.cpp: a silent run means nothing
// unless the same build path reports a race where there is one.
//
// A plain race on a global, which only instrumented loads and stores can
// see: without the sanitize_thread mark wtc-cc puts on every function, the
// first control is silent. Then the queue: q2 has two producers and two
// consumers, and forcing the spsc ring on it races.
//
// REQUIRES: tsan
// RUN: %wtc_cc --tsan -O1 -g %s -o %t.plain.exe
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 not %t.plain.exe 2>&1 | FileCheck %s
// RUN: %wtc_cc --tsan -O1 -g --force-spsc q2 %examples/queue_example.cpp -o %t.qe.exe
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 not %t.qe.exe 2>&1 | FileCheck %s
//
// CHECK: WARNING: ThreadSanitizer: data race

#include "thread.hpp"

long counter;

void bump() {
    for (int i = 0; i < 1000; ++i)
        counter = counter + 1;
}

int main() {
    std::thread a = wtc::spawn(bump);
    std::thread b = wtc::spawn(bump);
    a.join();
    b.join();
    return counter == 0;
}
