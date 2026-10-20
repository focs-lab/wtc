// One helper pops for every stage, each calling it with its own queue: the
// pop may act on either queue, and is checked as a pop of each. Its
// consumer copies the value out and lets the shared_ptr go before the next
// pop, so both queues are borrowed and become inline rings. Before such a
// pop was charged to each of its queues it was charged to none, and no
// queue of its type could be inline (found on a port of PARSEC dedup,
// whose stages all take through one helper).
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=CC
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
//
// CHECK-DAG: queue @first: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @second: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @first: every pop only borrows its element (1 pop)
// CHECK-DAG: queue @second: every pop only borrows its element (1 pop)
// CHECK-COUNT-2: specialised: constructor tag rewritten to spsc_inline
// CC-COUNT-2: specialised: constructor tag rewritten to spsc_inline
// TSAN-NOT: ThreadSanitizer
// OUT: sum 110

#include "queue.hpp"
#include "thread.hpp"

#include <cstdio>
#include <thread>

wtc::queue<int> first, second;

static int take(wtc::queue<int> *q) {
    for (;;)
        if (auto e = q->try_pop()) return *e;
}

int sum;
void stage1() { for (int i = 0; i < 10; ++i) second.push(take(&first) * 2); }
void stage2() { for (int i = 0; i < 10; ++i) sum += take(&second); }

int main() {
    std::thread a = wtc::spawn(stage1);
    std::thread b = wtc::spawn(stage2);
    for (int i = 1; i <= 10; ++i) first.push(i);
    a.join();
    b.join();
    std::printf("sum %d\n", sum);
}
