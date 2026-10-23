// A declared spsc queue that a push may or may not act on: the push goes
// through a field that holds one of two queues, so whether it is a second
// producer of q is not certain. That is not a proof of a broken promise:
// the build goes on with a warning, and q, counted with the push, keeps
// the general-purpose implementation's verdict.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: error:
// CHECK: warning: wtc: queue @q is declared spsc; cannot verify: an operation may act on it or on another queue

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q(wtc::topology::spsc);
wtc::queue<int> r;

struct handle { wtc::queue<int>* to; };
handle h;
int which;

void forwarder() { h.to->push(1); }
void producer() { q.push(2); }

int main() {
    if (which) h.to = &q;
    else h.to = &r;
    std::thread a = wtc::spawn(forwarder);
    std::thread b = wtc::spawn(producer);
    q.try_pop();
    r.try_pop();
    a.join();
    b.join();
}
