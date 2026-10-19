// Two structs of one type: one constructor call builds the queue in both,
// and it passes one tag. Each queue is spsc, but rewriting the call would
// decide for both at once, and the analysis does not take a verdict about
// one object as one about another: both keep the general-purpose
// implementation, and say why.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: queue @a.q: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @b.q: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @a.q keeps the general-purpose implementation: its constructor call also builds other queues
// CHECK-DAG: queue @b.q keeps the general-purpose implementation: its constructor call also builds other queues
// CHECK-NOT: tag rewritten

#include "queue.hpp"
#include "thread.hpp"

struct channel { wtc::queue<int> q; };
channel a, b;

void pa() { a.q.push(1); }
void pb() { b.q.push(2); }

int main() {
    std::thread x = wtc::spawn(pa);
    std::thread y = wtc::spawn(pb);
    a.q.try_pop();
    b.q.try_pop();
    x.join();
    y.join();
}
