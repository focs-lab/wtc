// A thread started on a member function of a library queue,
// std::thread(&rigtorp::SPSCQueue<int>::push, &q, v), pushes through a
// member-function pointer no call site shows. Two such threads are two
// producers the walk never sees, so the type's queues are unknown rather
// than verified.
//
// RUN: %clangxx -std=c++17 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: remark: wtc: the address of rigtorp::SPSCQueue<int, std::allocator<int>>::push(int const&) is taken, and calls through it are not counted; every queue of its type is now unknown
// CHECK: warning: wtc: queue @q is declared spsc (by its type); cannot verify: the address of rigtorp::SPSCQueue<int, std::allocator<int>>::push(int const&) is taken
// CHECK-NOT: verified

#include "spsc_libs.hpp"
#include <thread>

rigtorp::SPSCQueue<int> q(16);

void consumer() {
    if (q.front()) q.pop();
}

int main() {
    std::thread a(&rigtorp::SPSCQueue<int>::push, &q, 1), b(&rigtorp::SPSCQueue<int>::push, &q, 2), c(consumer);
    a.join(); b.join(); c.join();
}
