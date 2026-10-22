// A thread started with std::thread and a function, with or without
// arguments. The function's address goes into the constructor's callable
// operand and nowhere else, so it is not a function that may run anywhere:
// the thread runs it, through the indirect call in the library's invoker,
// and the call is resolved back to the constructor's operand. The same
// holds through a wrapper that passes the function on as a parameter.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: address of this function is taken
// CHECK-NOT: could not be resolved
// CHECK-DAG: queue @plain: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @args: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @wrapped: spsc (producers: 1, consumers: 1)
// CHECK-NOT: address of this function is taken
// CHECK-NOT: could not be resolved

#include "queue.hpp"

#include <thread>

wtc::queue<int> plain, args, wrapped;

void fill_plain() { plain.push(1); }
void fill_args(int n, long m) { args.push(n + static_cast<int>(m)); }
void fill_wrapped() { wrapped.push(1); }

std::thread start(void (*f)()) { return std::thread(f); }

int main() {
    std::thread t(fill_plain);
    std::thread u(fill_args, 1, 2L);
    std::thread w = start(fill_wrapped);
    while (!plain.try_pop()) {}
    while (!args.try_pop()) {}
    while (!wrapped.try_pop()) {}
    t.join();
    u.join();
    w.join();
}
