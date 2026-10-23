// A worker pool: the spawn sits inside a loop, so one static call site
// stands for many threads. The analysis must say mpsc, not spsc, and the
// constructor tag must stay generic. The program's output is the same
// either way; what this protects is that the compiler does not build the
// racing version.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize 2>&1 >%t.wtc.cir | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.wtc.cir
// RUN: grep -B1 'queueIiEC[12]ENS_8topologyE' %t.wtc.cir | FileCheck %s --check-prefix=CTOR
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe
// RUN: %t.exe
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --allow-empty --check-prefix=TSAN %}
//
// TSAN-NOT: ThreadSanitizer
// CHECK: queue @pool: mpsc (producers: many, consumers: 1)
// CHECK-NOT: specialised: constructor tag rewritten
// IR-NOT: wtc.specialized
// The one constructor call keeps its generic tag.
// CTOR: cir.const #cir.int<0> : !s32i
// CTOR-NEXT: cir.call @_ZN3wtc5queueIiEC{{[12]}}ENS_8topologyE

#include "queue.hpp"
#include "thread.hpp"

#include <thread>
#include <vector>

wtc::queue<int> pool;

void worker() {
    for (int i = 0; i < 1000; ++i) pool.push(i);
}

int main() {
    std::vector<std::thread> workers;
    for (int k = 0; k < 4; ++k) workers.push_back(wtc::spawn(worker));
    int got = 0;
    while (got < 4000) {
        if (pool.try_pop()) ++got;
    }
    for (auto& w : workers) w.join();
    return got == 4000 ? 0 : 1;
}
