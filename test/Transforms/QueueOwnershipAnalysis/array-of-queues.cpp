// An array of queues: one constructor call builds every element, and the
// analysis does not tell the elements apart, so it cannot say which
// element a thread's operation acts on. Every queue of the type is unknown.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: queue access is unresolvable
// CHECK-NOT: tag rewritten

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> qs[2];
int which;

void producer() { qs[which].push(1); }

int main() {
    std::thread t = wtc::spawn(producer);
    qs[0].try_pop();
    t.join();
}
