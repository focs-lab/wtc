// The same two stages without std::thread: locals of main, workers started
// with pthread_create in the constructor's loop and handed `this`. The
// source's workers push into `sink.q_`; their loop is the one in the
// source's constructor run, not the sink's, and the sink's constructor is
// where the choice would be read. The loop is not credited, whether the
// walk names the two objects apart or by their record, and the sink's
// queue keeps its static verdict.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>%t.remarks
// RUN: FileCheck %s < %t.remarks
// RUN: FileCheck %s --check-prefix=NONE < %t.remarks
//
// CHECK: queue {{q_ in any stage|%sink.q_}}: mpsc (producers: many, consumers: 1)
// NONE-NOT: one thread per iteration
// NONE-NOT: specialised at run time

#include "queue.hpp"

#include <pthread.h>
#include <vector>

constexpr int kPer = 1000;

struct stage {
    wtc::queue<int> q_;
    std::vector<pthread_t> ts_;
    stage *next_;
    static void *run(void *p) {
        static_cast<stage *>(p)->loop();
        return nullptr;
    }
    stage(int n, stage *next) : next_(next) {
        ts_.resize(n);
        for (int i = 0; i < n; i++) pthread_create(&ts_[i], nullptr, run, this);
    }
    ~stage() { for (auto &t : ts_) pthread_join(t, nullptr); }
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
