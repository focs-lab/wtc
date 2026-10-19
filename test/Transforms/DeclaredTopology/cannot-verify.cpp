// A producer handed to code the module cannot see may run on any thread,
// any number of times, so the declaration can be neither verified nor
// refuted: a warning says why, and the build goes on.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: warning: wtc: queue @q is declared spsc; cannot verify: a thread that uses it is one the module cannot account for
// CHECK-NOT: error

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q(wtc::topology::spsc);

void on_event() { q.push(1); }
void register_callback(void (*)());  // defined elsewhere

void reader() { while (!q.try_pop()) {} }

int main() {
    register_callback(on_event);
    std::thread r = wtc::spawn(reader);
    r.join();
}
