// One helper takes two queues as parameters and pushes to the second only,
// and another takes its queue as parameter 1 rather than 0. Resolving a
// parameter by its index through the call site of this particular instance
// must credit b and not a. An analysis that always reads operand zero
// would give a the producer that belongs to b.
//
// The second queue reaches the worker through spawn's void* argument, which
// the compiler passes through a cast and a stack slot; that is the shape the
// single-store forwarding rule exists for.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s
//
// CHECK-DAG: queue @a: spsc (producers: 0, consumers: 1)
// CHECK-DAG: queue @b: spsc (producers: 1, consumers: 1)

#include "queue.hpp"
#include "thread.hpp"

#include <thread>

wtc::queue<int> a;
wtc::queue<int> b;

void push_to_second(wtc::queue<int>* first, wtc::queue<int>* second) {
    (void)first;
    for (int i = 0; i < 10; ++i) second->push(i);
}

void pop_from(int tag, wtc::queue<int>* q) {
    (void)tag;
    q->try_pop();
}

void worker(void* arg) {
    auto* second = static_cast<wtc::queue<int>*>(arg);
    push_to_second(&a, second);
}

int main() {
    std::thread t = wtc::spawn(worker, &b);
    pop_from(1, &a);
    pop_from(2, &b);
    t.join();
    return 0;
}
