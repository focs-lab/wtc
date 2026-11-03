// A pool whose queue is a member of a member, as spdlog's lifted pool
// holds it: the pool's member is a queue object (spdlog's
// mpmc_blocking_queue), whose constructor builds the queue, and the
// workers the pool's constructor starts in a counted loop pop
// `this->inner_.q_` through a method of each. Two things stood between
// that and the choice the pool makes when its queue is a direct member:
// the workers' pops were read only up to the queue object's `this`, so
// they were not credited to the loop that started them, and the queue's
// constructor is one call below the loop's function, so the tag could not
// be chosen there. Now the pops are followed up through the members to
// the captured `this`, the constructor is followed down through the one
// call that builds the member, and the call's function is copied with the
// tag as a parameter, chosen just before the call.
//
// What must not pass (no run-time choice): the workers pop another pool's
// queue (OTHER); the pool builds two queue objects, so the one call that
// builds the popped one cannot be named (TWO).
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o - 2>%t.remarks | FileCheck %s --check-prefix=IR
// RUN: FileCheck %s < %t.remarks
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DOTHER %s -o %t.other.cir
// RUN: wtc-opt %t.other.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=NONE
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DTWO %s -o %t.two.cir
// RUN: wtc-opt %t.two.cir --mlir-print-op-on-diagnostic=false --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=NONE
//
// CHECK: queue q_ in any box: spmc (producers: 1, consumers: many)
// CHECK: queue q_ in any box: consumers: one thread per iteration of the loop at {{.*}}, so one when it runs at most once
// CHECK: specialised at run time: spsc{{(_inline)?}} when the loop at {{.*}} (consumers) runs at most once, else spmc; chosen at the call at {{.*}}, which builds it
// IR: cir.func {{.*}}@_ZN4poolC2EiPS_(
// IR: cir.select
// IR: cir.call @_ZN3boxC2Ev.wtc_tag(
// IR: cir.func {{.*}}internal {{.*}}@_ZN3boxC2Ev.wtc_tag(
//
// NONE-NOT: one thread per iteration
// NONE-NOT: specialised at run time

#include "queue.hpp"

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
#ifdef TWO
    box spare_;
#endif
    pool *next_;
    std::vector<std::thread> ws_;
    pool(int n, pool *next) : next_(next) {
        for (int i = 0; i < n; i++) ws_.emplace_back([this] { this->loop(); });
    }
    ~pool() {
        for (auto &t : ws_) t.join();
    }
    void loop() {
#ifdef OTHER
        box &from = next_->inner_;
#else
        box &from = inner_;
#endif
        for (int v = 1; v != 0;)
            if (!from.take(v)) v = 1;
    }
};

int main(int argc, char **) {
    pool sink(0, nullptr);
    pool p(argc, &sink);
#ifdef OTHER
    pool &to = sink;
#else
    pool &to = p;
#endif
    for (int i = 1; i < 100; ++i) to.inner_.put(i);
    for (int i = 0; i < argc; ++i) to.inner_.put(0);
}
