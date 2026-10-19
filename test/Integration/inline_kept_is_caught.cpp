// The test that proves the guard can fire: a consumer that keeps the
// element it popped past its next pop, on a queue tagged spsc_inline, must
// abort under WTC_CHECK with the guard's message, not read a reused slot.
//
// RUN: %clangxx -std=c++17 -I%shim -DWTC_CHECK %s -o %t.exe
// RUN: not --crash %t.exe 2>&1 | FileCheck %s
//
// CHECK: wtc: borrowed element still referenced at the next try_pop on queue

#include "queue.hpp"

struct element {
    long v;
};

int main() {
    wtc::queue<element> q(wtc::topology::spsc_inline);
    q.push(element{1});
    q.push(element{2});
    auto kept = q.try_pop();
    auto next = q.try_pop();
    return static_cast<int>(kept->v + next->v);
}
