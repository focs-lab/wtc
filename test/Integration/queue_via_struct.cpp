// The queues reach the worker inside a struct: the push goes through a
// load of a member, not through a stack slot written once. The member's
// value is read from every write of that member in the module -- here the
// one constant the front end copies `pair p{&a, &b}` out of -- so the push
// is on @b, and both queues are proved spsc and specialised.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize 2>&1 >%t.wtc.cir | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.wtc.cir
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=CC
// RUN: %t.exe
//
// CHECK-NOT: queue access is unresolvable
// CHECK-DAG: queue @a: spsc (producers: 0, consumers: 1)
// CHECK-DAG: queue @b: spsc (producers: 1, consumers: 1)
// CHECK-COUNT-2: specialised: constructor tag rewritten to spsc
// IR-DAG: cir.global{{.*}}@a{{.*}}wtc.specialized
// IR-DAG: cir.global{{.*}}@b{{.*}}wtc.specialized
// CC-NOT: unresolvable

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
