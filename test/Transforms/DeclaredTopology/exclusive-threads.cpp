// A declared spsc queue pushed to from two thread functions, one started
// on each arm of an if: at most one of the two threads exists, so there
// is no proof of two producers and the build goes on, with a warning that
// says why the declaration cannot be verified. The count of producers,
// which does not tell exclusive threads apart, still says many.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: error:
// CHECK: warning: wtc: queue @q is declared spsc; cannot verify: the threads that push to it are started on different arms of one branch, so at most one of them exists, which the count does not tell

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q(wtc::topology::spsc);
int which;

void t1() { q.push(1); }
void t2() { q.push(2); }

int main() {
    std::thread t;
    if (which) t = wtc::spawn(t1);
    else t = wtc::spawn(t2);
    q.try_pop();
    t.join();
}
