// A consumer step declared lock-free fails on either queue that has a side
// with many threads: the queue for many producers and the queue for many
// consumers both free the segments their pop moves past, and the former may
// also allocate to note cells it passes. Neither waits for another thread:
// the spmc pop's claim is lock-free, which a lock-free step allows, so only
// freeing segments and the shared_ptr are reported for it.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: specialised: constructor tag rewritten to mpsc
// CHECK-DAG: specialised: constructor tag rewritten to spmc
// CHECK-DAG: error: wtc: take_in() is declared lock-free but can block
// CHECK-DAG: note: which frees the segments it moves past, and may allocate to note the cells it passes that a push has taken and not filled
// CHECK-DAG: error: wtc: take_out() is declared lock-free but can block
// CHECK-DAG: note: which frees the segments it moves past, and allocates the shared_ptr it returns

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> fan_in;
wtc::queue<int> fan_out;

WTC_LOCK_FREE bool take_in() { return fan_in.try_pop() != nullptr; }
WTC_LOCK_FREE bool take_out() { return fan_out.try_pop() != nullptr; }

void in_producer() { fan_in.push(1); }
void in_consumer() { while (!take_in()) {} }
void out_producer() { fan_out.push(1); fan_out.push(2); }
void out_consumer() { while (!take_out()) {} }

int main() {
    std::thread a = wtc::spawn(in_producer), b = wtc::spawn(in_producer);
    std::thread c = wtc::spawn(in_consumer);
    std::thread p = wtc::spawn(out_producer);
    std::thread x = wtc::spawn(out_consumer), y = wtc::spawn(out_consumer);
    a.join(); b.join(); c.join(); p.join(); x.join(); y.join();
}
