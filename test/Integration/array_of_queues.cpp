// A pipeline whose stage queues are elements of an array on the heap, each
// stage finding its queue by an index it computes, as PARSEC dedup does:
// every element is built by one placement new in a loop, and every access
// uses an index the analysis cannot read. The elements are counted
// together: one producer thread and one consumer thread for each stage
// would be two of each over the array, so the array keeps the general
// queue; here one thread fills every queue and one drains them, which over
// the union is one of each, and every element becomes the single-producer
// ring. WTC_CHECK traps a second producer or consumer on a ring, and the
// ThreadSanitizer build reports any race the substitution let in.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --allow-empty --check-prefix=TSAN %}
//
// CHECK: queue *@stages[*]: spsc (producers: 1, consumers: 1)
// CHECK: specialised: constructor tag rewritten to spsc
// CHECK-NOT: unresolvable
// OUT: sum 4950
// TSAN-NOT: ThreadSanitizer

#include "queue.hpp"
#include "thread.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>

constexpr int kStages = 4;
wtc::queue<int> *stages;
int rounds = 25;

// The queue a value goes to, computed from the value: the analysis cannot
// read it, and any element may be named.
static int stageOf(int v) { return v % kStages; }

void fill() {
    for (int v = 0; v < rounds * kStages; ++v) stages[stageOf(v)].push(v);
}

long drained = 0;
void drain() {
    for (int s = 0; s < kStages; ++s)
        for (int got = 0; got < rounds;) {
            int v;
            if (stages[s].try_pop(v)) { drained += v; ++got; }
        }
}

int main() {
    stages = static_cast<wtc::queue<int> *>(std::malloc(sizeof(wtc::queue<int>) * kStages));
    for (int s = 0; s < kStages; ++s) new (&stages[s]) wtc::queue<int>;
    std::thread producer = wtc::spawn(fill);
    std::thread consumer = wtc::spawn(drain);
    producer.join();
    consumer.join();
    std::printf("sum %ld\n", drained);
    for (int s = 0; s < kStages; ++s) stages[s].~queue();
    std::free(stages);
}
