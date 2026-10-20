// A pop through a field that holds one of two queues names neither, so
// what it does with its element cannot be charged to one queue: both are
// proved spsc, and neither is borrowed, whatever their other pops do. The
// consumer here keeps what it pops, which would read a reused slot if
// either queue held its elements inline.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: queue @a: spsc
// CHECK-DAG: queue @b: spsc
// CHECK-DAG: queue @a: not borrowed: a pop that may act on several queues of its type was not checked
// CHECK-DAG: queue @b: not borrowed: a pop that may act on several queues of its type was not checked

#include "queue.hpp"
#include "thread.hpp"

#include <memory>

wtc::queue<int> a;
wtc::queue<int> b;

struct handle { wtc::queue<int>* q; };
handle h;
int which;
std::shared_ptr<int> kept;

void consumer() {
    for (int i = 0; i < 10; ++i) kept = h.q->try_pop();
}

int main() {
    if (which) h.q = &a;
    else h.q = &b;
    std::thread t = wtc::spawn(consumer);
    a.push(1);
    b.push(2);
    t.join();
}
