// REQUIRES: clangir, cir-opt
// RUN: %cir_cxx %s -o %t.cir
// RUN: wtc-opt %t.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis --select-queue-impl --lower-wtc-to-cir --reconcile-unrealized-casts -o %t.out.cir 2> %t.remarks > /dev/null
// RUN: FileCheck %s --check-prefix=REMARK < %t.remarks
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir
// RUN: cir-opt %t.out.cir --cir-to-llvm -o /dev/null

// The whole pipeline, from C++ to the LLVM dialect, on a program with one
// SPSC queue and one MPMC queue of the same type. Nothing here names the
// substitute. The SPSC queue is substituted and the MPMC one is not; the
// anchor that made the substitute available is gone; and the result is
// consistent enough for CIR to lower it. Nothing is run.

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> single;
wtc::queue<int> shared;

void produce_single() { single.push(1); }
void consume_single() { (void)single.try_pop(); }
void produce_shared() { shared.push(2); }
void consume_shared() { (void)shared.try_pop(); }

int main() {
    auto a = wtc::spawn(produce_single);
    auto b = wtc::spawn(consume_single);
    auto c = wtc::spawn(produce_shared);
    auto d = wtc::spawn(produce_shared);
    auto e = wtc::spawn(consume_shared);
    auto f = wtc::spawn(consume_shared);
    a.join(); b.join(); c.join(); d.join(); e.join(); f.join();
}

// REMARK-DAG: remark: substituted "wtc::runtime::spsc_queue<int, 1024UL>"
// REMARK-DAG: remark: not substituted: the queue is MPMC

// IR-NOT: wtc_anchor
// IR-DAG: cir.global external @single = #cir.zero : !rec_wtc3A3Aruntime3A3Aspsc_queue
// IR-DAG: cir.global external @shared = #cir.zero : !rec_wtc3A3Aqueue
