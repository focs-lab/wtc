// A section that takes this object's mutex and lets go another object's
// mutex of the same member: the pass cannot tell the two apart, and the
// server would let go a mutex its client never took. The section that
// takes and lets go its own object's mutex is delegated.
//
// RUN: %clangxx -std=c++17 -O1 -fno-exceptions -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=S::mu_ -o /dev/null 2>&1 | FileCheck %s

#include <mutex>

struct S {
    std::mutex mu_;
    long n = 0;
    // CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section S::handoff #1 on S::mu_ not delegated: (b) lets go a mutex of the same member that the pass cannot tell is the one it took
    void handoff(S *o) noexcept {
        mu_.lock();
        ++n;
        o->mu_.unlock();
    }
    // CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section S::own #1 on S::mu_ delegated
    void own() noexcept {
        std::lock_guard<std::mutex> g(mu_);
        ++n;
    }
};

int main() {
    S s, t;
    t.mu_.lock();
    s.handoff(&t);
    s.own();
    return 0;
}
