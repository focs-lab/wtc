// A constructor defined in the unit, out of line. The front end emits the
// complete-object constructor (C1), which `worker w;` calls, as an alias
// of the base-object one (C2), with no body of its own. Read as a call of
// a bodiless function, the call handed the queue member to code the
// analysis cannot see, and every queue of its type escaped: g3log's
// LogWorkerImpl, whose constructor starts the worker of its queue, was
// unknown for that reason. A call of an alias is a call of its aliasee.
//
// The alias's own address is still the aliasee's: a function reached only
// through a pointer to its alias runs wherever that pointer is called
// (TAKEN), and its pops are counted on a thread that may be any.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
// A weak alias is not followed: the link may put another definition in
// its place, which may do anything with what it is handed (WEAK). A call
// of it is a call of a function with no body here.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DWEAK %s -o %t.weak.cir
// RUN: wtc-opt %t.weak.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=WEAK
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DTAKEN %s -o %t.taken.cir
// RUN: wtc-opt %t.taken.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=TAKEN
//
// CHECK-NOT: escapes
// CHECK: queue q in any worker: spsc (producers: 1, consumers: 1)
// CHECK-NOT: escapes
//
// WEAK: a queue escapes this translation unit (it is handed to take, whose body is not here)
// WEAK: queue %q: unknown (producers: 1, consumers: 0)
//
// TAKEN: queue @g: spmc (producers: 1, consumers: many)
// TAKEN: queue @g: many consumers: {{.*}}real_drain may run any number of times: its address is taken

#include "queue.hpp"

#include <thread>

#if defined(WEAK)
// Does nothing here; the program may link another `take` that pops.
extern "C" void no_take(wtc::queue<int> *) {}
extern "C" void take(wtc::queue<int> *) __attribute__((weak, alias("no_take")));

int main() {
    wtc::queue<int> q;
    std::thread a([&] { take(&q); });
    std::thread b([&] { take(&q); });
    for (int i = 0; i < 3; ++i) q.push(i);
    a.join();
    b.join();
}
#elif !defined(TAKEN)
struct worker {
    wtc::queue<int> q;
    std::thread t;
    int got = 0;
    worker();
    ~worker();
    void loop() {
        while (got < 3)
            if (q.try_pop()) ++got;
    }
};

worker::worker() : t(&worker::loop, this) {}
worker::~worker() { t.join(); }

int main() {
    worker w;
    for (int i = 0; i < 3; ++i) w.q.push(i);
}
#else
wtc::queue<int> g;

// Called here, so no root for want of a caller: only the alias's address
// makes it one.
extern "C" void real_drain() {
    for (int got = 0; got < 3;)
        if (g.try_pop()) ++got;
}
extern "C" void drain() __attribute__((alias("real_drain")));

int main() {
    void (*volatile entry)() = drain;
    std::thread t(entry);
    for (int i = 0; i < 6; ++i) g.push(i);
    real_drain();
    t.join();
}
#endif
