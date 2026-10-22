// A queue handed in by reference to a function nothing in this translation
// unit calls: the parameter is a root the walk cannot place, and the
// handle is not a member of any record either, so there is no queue to
// count the push on. The access is unresolvable, with that reason, and
// every queue of the type is unknown, the global included.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: queue access is unresolvable (the queue object could not be identified); every queue of this type is now unknown
// CHECK: queue @g: unknown

#include "queue.hpp"

wtc::queue<int> g;

void feed(wtc::queue<int> &q) { q.push(1); }

int main() { g.try_pop(); }
