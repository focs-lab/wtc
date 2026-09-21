// What the analysis cannot resolve, it must call unknown, and the rewrite
// must not happen. Here the queues reach the worker inside a struct, so the
// pushes go through a load of a member rather than a stack slot written
// once. That is beyond the MVP's tracing, and the correct answer for it is
// not a guess: every queue of this type becomes unknown, no constructor is
// rewritten, and the program keeps the general-purpose implementation.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize 2>&1 >%t.wtc.cir | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.wtc.cir
//
// CHECK: queue access is unresolvable
// CHECK-DAG: queue @a: unknown
// CHECK-DAG: queue @b: unknown
// CHECK-NOT: specialised: constructor tag rewritten
// IR-NOT: wtc.specialized

#include "queue.hpp"
#include "thread.hpp"

#include <thread>

wtc::queue<int> a;
wtc::queue<int> b;

struct pair { wtc::queue<int>* x; wtc::queue<int>* y; };

void worker(void* arg) {
    auto* p = static_cast<pair*>(arg);
    for (int i = 0; i < 10; ++i) p->y->push(i);
}

int main() {
    pair p{&a, &b};
    std::thread t = wtc::spawn(worker, &p);
    a.try_pop();
    b.try_pop();
    t.join();
    return 0;
}
