// A handle loaded from a field written in two places names one of two
// queues, and the push through it is counted on both. Here the worker
// pushes through the field, which holds &a or &b, and main pushes to a
// itself: a has two producers whichever the field holds, so it must not be
// called spsc; b has the worker alone. The push names no queue, since it
// has two.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o %t.wtc.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.wtc.cir
//
// CHECK-NOT: unresolvable
// CHECK-DAG: queue @a: mpsc (producers: many, consumers: 1)
// CHECK-DAG: queue @b: spsc (producers: 1, consumers: 1)
// IR: "wtc.queue_push"{{.*}}<{fallback_impl = @{{[^,]*}}, source_arg_attrs{{.*}}topology = #wtc.topology<unknown>

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> a;
wtc::queue<int> b;

struct handle { wtc::queue<int>* q; };
handle h;
int which;

void worker() { for (int i = 0; i < 10; ++i) h.q->push(i); }

int main() {
    if (which) h.q = &a;
    else h.q = &b;
    std::thread t = wtc::spawn(worker);
    a.push(0);
    a.try_pop();
    b.try_pop();
    t.join();
}
