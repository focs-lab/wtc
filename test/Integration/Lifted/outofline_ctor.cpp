// A finding of the review of 23 Nov 2026: a constructor defined out of
// line, whose complete-object symbol is an alias of the base-object one: it
// has the aliasee's body, and the class lifts (a false refusal before).
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: popped 1
// WHY: locked queue q_ in LQ2 lifted (

#include "Inputs/lq.h"
struct LQ2 {
    std::queue<int> q_;
    std::mutex m_;
    std::condition_variable cv_;
    LQ2();
    void push(int v) { { std::lock_guard<std::mutex> l(m_); q_.push(v); } cv_.notify_one(); }
    void pop(int &out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return !q_.empty(); });
        out = q_.front(); q_.pop();
    }
};
LQ2::LQ2() {}
LQ2 g;
int main() { g.push(1); int v; g.pop(v); std::printf("popped %d\n", v); }
