// A push reached through recursion makes every queue of its type unknown,
// and no other. The recursive function here is entered from two call
// sites, so the walk meets the recursion twice and poisons the same push
// twice; the second time once counted as an access with no type, which
// made every queue in the program unknown, @flat below included, which
// nothing recursive touches. Found on a port of PARSEC ferret, whose loader
// scans directories recursively.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: remark: wtc: queue access is unresolvable (reached through recursion)
// CHECK-NOT: unresolvable
// CHECK-NOT: no type to attribute
// CHECK-DAG: queue @deep: unknown
// CHECK-DAG: queue @flat: spsc (producers: 1, consumers: 1)

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> deep;
wtc::queue<long> flat;

void walk(int depth) {
    if (depth == 0) return;
    deep.push(depth);
    walk(depth - 1);
}

void load() {
    walk(3);
    walk(2);
}
void drain() { for (int got = 0; got < 5;) if (deep.try_pop()) ++got; }
void send() { flat.push(1); }
void take() { while (!flat.try_pop()) {} }

int main() {
    std::thread a = wtc::spawn(load);
    std::thread b = wtc::spawn(drain);
    std::thread c = wtc::spawn(send);
    std::thread d = wtc::spawn(take);
    a.join(); b.join(); c.join(); d.join();
}
