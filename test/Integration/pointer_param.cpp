// A queue reached only through a pointer parameter, from real compiler
// output. At any optimisation level the parameter is stored into a stack
// slot in the entry block and loaded at each use, so resolving it needs the
// single-store forwarding rule; hand-written tests use bare block arguments,
// a shape the compiler never emits, and passing those proves nothing about
// this.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize 2>&1 >%t.wtc.cir | FileCheck %s
// RUN: grep -B1 'queueIiEC[12]ENS_8topologyE' %t.wtc.cir | FileCheck %s --check-prefix=CTOR
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe
// RUN: %t.exe
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --allow-empty --check-prefix=TSAN %}
//
// TSAN-NOT: ThreadSanitizer
// CHECK: queue @shared: spsc (producers: 1, consumers: 1)
// CHECK: specialised: constructor tag rewritten to spsc
// CTOR: cir.const #cir.int<1> : !s32i
// CTOR-NEXT: cir.call @_ZN3wtc5queueIiEC{{[12]}}ENS_8topologyE

#include "queue.hpp"
#include "thread.hpp"

#include <thread>

wtc::queue<int> shared;

void produce(void* arg) {
    auto* q = static_cast<wtc::queue<int>*>(arg);
    for (int i = 0; i < 100; ++i) q->push(i);
}

void consume(void* arg) {
    auto* q = static_cast<wtc::queue<int>*>(arg);
    int got = 0;
    while (got < 100) if (q->try_pop()) ++got;
}

int main() {
    std::thread p = wtc::spawn(produce, &shared);
    std::thread c = wtc::spawn(consume, &shared);
    p.join();
    c.join();
    return 0;
}
