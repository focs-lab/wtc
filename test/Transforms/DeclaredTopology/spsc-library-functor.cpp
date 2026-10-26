// Program code that only a library body calls: boost's consume_one and
// consume_all run the functor they are given on the consumer's thread.
// The functor's pushes are the consumer thread's, so `out` and `lib_out`
// each have two producers, `a` and `b`. A shim queue there must not be
// found spsc (that would rewrite it to a ring with two producers), and a
// library queue declared spsc there is broken.
//
// RUN: %clangxx -std=c++17 -I%shim -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: remark: wtc: queue @out: mpsc (producers: many, consumers: 1)
// CHECK-DAG: error: wtc: queue @lib_out is declared spsc (by its type) but 2 threads push to it
// CHECK-DAG: remark: wtc: queue @in: declared spsc (by its type), verified in this translation unit
// CHECK-NOT: queue @out: spsc

#include "queue.hpp"
#include "thread.hpp"
#include "spsc_libs.hpp"

boost::lockfree::spsc_queue<int> in(16);
wtc::queue<int> out;
rigtorp::SPSCQueue<int> lib_out(16);

void feeder() { in.push(1); in.push(2); }
void a() { out.push(0); lib_out.push(0); }
void b() {
    in.consume_one([](int v) { out.push(v); });
    in.consume_all([](int v) { lib_out.push(v); });
}
void c() {
    while (!out.try_pop()) {}
    if (lib_out.front()) lib_out.pop();
}

int main() {
    std::thread f = wtc::spawn(feeder), ta = wtc::spawn(a), tb = wtc::spawn(b), tc = wtc::spawn(c);
    f.join(); ta.join(); tb.join(); tc.join();
}
