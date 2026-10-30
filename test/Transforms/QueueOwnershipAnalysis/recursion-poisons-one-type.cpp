// What recursion makes unknown, and what it does not. The walk expands a
// recursive function's first recurrence once more and stops there, so a
// handle that the recursion binds through its own parameters may name, at
// a depth the walk did not reach, an object it never saw: such a push is
// unresolvable, and every queue of its type is unknown (@deep, handed down
// by reference). A handle fixed below the recursion names the same queue
// at every depth, on the same thread, and is counted: a global pushed to
// directly (@direct), a global handed to a helper outside the cycle, as
// PARSEC ferret's loader hands its first queue to enqueue from the file
// helper its recursive directory scan calls (@below), and a queue a
// pointer global holds, written once, as ferret's port keeps it
// (*@held). The recursive functions are entered from two call sites
// each, so the walk meets the recursion twice; the second sighting once
// counted as an access with no type, which made every queue in the
// program unknown.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: remark: wtc: queue access is unresolvable (reached through recursion)
// CHECK-NOT: unresolvable
// CHECK-NOT: no type to attribute
// CHECK-DAG: queue @deep: unknown
// CHECK-DAG: queue @direct: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @below: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue *@held: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @flat: spsc (producers: 1, consumers: 1)

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> deep;
wtc::queue<long> flat;
wtc::queue<short> direct;
wtc::queue<char> below;
wtc::queue<unsigned> *held;

void walk(wtc::queue<int> &q, int depth) {
    if (depth == 0) return;
    q.push(depth);
    walk(q, depth - 1);
}

void put(wtc::queue<char> &q, char v) { q.push(v); }

void scan(int depth) {
    if (depth == 0) return;
    direct.push(static_cast<short>(depth));
    put(below, static_cast<char>(depth));
    held->push(static_cast<unsigned>(depth));
    scan(depth - 1);
}

void load() {
    walk(deep, 3);
    walk(deep, 2);
    scan(3);
    scan(2);
}
void drain() {
    for (int got = 0; got < 5;)
        if (deep.try_pop()) ++got;
    for (int got = 0; got < 5;)
        if (direct.try_pop()) ++got;
    for (int got = 0; got < 5;)
        if (below.try_pop()) ++got;
    for (int got = 0; got < 5;)
        if (held->try_pop()) ++got;
}
void send() { flat.push(1); }
void take() { while (!flat.try_pop()) {} }

int main() {
    held = new wtc::queue<unsigned>();
    std::thread a = wtc::spawn(load);
    std::thread b = wtc::spawn(drain);
    std::thread c = wtc::spawn(send);
    std::thread d = wtc::spawn(take);
    a.join(); b.join(); c.join(); d.join();
}
