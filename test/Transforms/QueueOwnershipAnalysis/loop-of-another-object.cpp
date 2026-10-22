// Two objects of one record, built by one constructor under two call
// sites: a sink with no workers, and a source whose workers push into the
// sink's queue. The queue is `q_ in any stage`, and the source's workers
// are started by the loop in the source's constructor, one per
// iteration -- but the run-time choice would be read at the sink's
// constructor, whose own loop ran zero times, and would hand the sink an
// spsc ring with two producers. The loop is credited to a thread only
// when the run that started it is the constructor run of the object the
// thread touches: here the workers touch another object (`next_->q_`,
// not the captured `this`), so no loop is recorded and the verdict stays
// static. Before this rule the shape was poisoned, which was safe; the
// rule keeps a pool whose workers pop their own `this->q_`.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>%t.remarks
// RUN: FileCheck %s < %t.remarks
// RUN: FileCheck %s --check-prefix=NONE < %t.remarks
//
// CHECK: queue q_ in any stage: mpsc (producers: many, consumers: 1)
// NONE-NOT: one thread per iteration
// NONE-NOT: specialised at run time

#include "queue.hpp"

#include <thread>
#include <vector>

constexpr int kPer = 1000;

struct stage {
    wtc::queue<int> q_;
    std::vector<std::thread> ws_;
    stage *next_;
    stage(int n, stage *next) : next_(next) {
        for (int i = 0; i < n; i++) ws_.emplace_back([this] { this->loop(); });
    }
    ~stage() { for (auto &t : ws_) t.join(); }
    void loop() {
        for (int i = 1; i <= kPer; ++i) next_->q_.push(i);
        next_->q_.push(0);
    }
};

int main(int argc, char **) {
    const int workers = argc + 1;
    stage sink(0, nullptr);
    stage src(workers, &sink);
    long sum = 0, ends = 0;
    while (ends < workers) {
        auto p = sink.q_.try_pop();
        if (!p) continue;
        if (*p == 0) ++ends; else sum += *p;
    }
    return sum == 0;
}
