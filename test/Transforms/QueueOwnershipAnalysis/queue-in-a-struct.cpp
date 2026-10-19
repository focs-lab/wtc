// A queue that is a member of a larger object has no verdict of its own:
// the verdict and the rewrite are kept per object and its constructor, and
// the queue's constructor runs inside the struct's. Every queue of the type
// becomes unknown, the one first in its struct, at offset zero, included:
// the place found there is the struct, not the queue.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: queue access is unresolvable (the queue is part of a larger object)
// CHECK-NOT: tag rewritten

#include "queue.hpp"
#include "thread.hpp"

struct channel {
    wtc::queue<int> q;
    int sent = 0;
};
channel c;

void producer() { for (int i = 0; i < 10; ++i) c.q.push(i); }

int main() {
    std::thread t = wtc::spawn(producer);
    c.q.try_pop();
    t.join();
}
