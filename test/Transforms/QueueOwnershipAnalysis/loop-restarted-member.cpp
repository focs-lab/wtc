// A pool whose queue is a member of a member, built and given its workers
// by a method that is not the constructor: start(n) builds the queue
// object in place and starts n workers that pop it. A constructor runs
// once per object, a method may run twice on one: here start(3), then
// start(1) on the same pool, which builds the queue again under the first
// three workers. The count read at the second build is not theirs, so the
// workers are credited to the loop only when the queue's constructor ran
// once in the whole program, in that method. With start called once
// (ONCE) it still ran twice, the first time in the pool's constructor,
// before any worker: that is refused as well, which costs nothing but the
// run-time choice.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=NONE
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DONCE %s -o %t.once.cir
// RUN: wtc-opt %t.once.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=NONE
//
// NONE: queue q_ in any box: spmc (producers: 1, consumers: many)
// NONE-NOT: one thread per iteration
// NONE-NOT: specialised at run time

#include "queue.hpp"

#include <new>
#include <thread>
#include <vector>

struct box {
    wtc::queue<int> q_;
    box() : q_() {}
    void put(int v) { q_.push(v); }
    bool take(int &v) {
        auto p = q_.try_pop();
        if (!p) return false;
        v = *p;
        return true;
    }
};

struct pool {
    box inner_;
    std::vector<std::thread> ws_;
    void start(int n) {
        new (&inner_) box();
        for (int i = 0; i < n; i++) ws_.emplace_back([this] { this->loop(); });
    }
    ~pool() {
        for (auto &t : ws_) t.join();
    }
    void loop() {
        for (int v = 1; v != 0;)
            if (!inner_.take(v)) v = 1;
    }
};

int main(int argc, char **) {
    pool p;
    p.start(argc + 2);
#ifndef ONCE
    p.start(1);
#endif
    for (int i = 1; i < 100; ++i) p.inner_.put(i);
    for (int i = 0; i < argc + 3; ++i) p.inner_.put(0);
}
