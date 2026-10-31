// (f) A class lifts only when the module is the whole program as far as
// the class goes. Lifting changes what its member functions do with the
// object's storage, and a copy of them compiled in another unit as
// written would read that storage as the container it no longer holds:
// a module compiled to be linked with files the pass does not see
// (open-world, what wtc-cc -c passes), a module without main, a file
// linked with this one that names the class (a member function, or a
// function that takes the class), and a member function declared here and
// defined elsewhere all refuse. A list of other symbols leaves the class
// alone.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLOSED
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues="rewrite=false open-world=true" -o /dev/null 2>&1 | FileCheck %s --check-prefix=OPEN
// RUN: echo _ZN2LQIiE7pendingEv > %t.member
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues="rewrite=false named-elsewhere=%t.member" -o /dev/null 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: echo _Z6reportR2LQIiE > %t.param
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues="rewrite=false named-elsewhere=%t.param" -o /dev/null 2>&1 | FileCheck %s --check-prefix=PARAM
// RUN: printf '_Z6reportR3LQ2IiE\n_ZN4MyLQIiE4pushEi\nmalloc\n' > %t.other
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues="rewrite=false named-elsewhere=%t.other" -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLOSED
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -DNO_MAIN -o %t.nomain.cir
// RUN: wtc-opt %t.nomain.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOMAIN
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -DOUT_OF_LINE -o %t.ool.cir
// RUN: wtc-opt %t.ool.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=OOL
//
// CLOSED: locked queue q_ in LQ<int> lifts
// CLOSED-NOT: not lifted
// OPEN: locked queue q_ in LQ<int> not lifted: (f) the unit is compiled to be linked with files the pass does not see, which may use LQ<int> as written
// MEMBER: locked queue q_ in LQ<int> not lifted: (f) a file linked with this one names LQ<int> (LQ<int>::pending())
// PARAM: locked queue q_ in LQ<int> not lifted: (f) a file linked with this one names LQ<int> (report(LQ<int>&))
// NOMAIN: locked queue q_ in LQ<int> not lifted: (f) the module has no main: the rest of the program, elsewhere, may use LQ<int> as written
// OOL: locked queue q_ in LQ<int> not lifted: (f) LQ<int>::pending has no body here: another unit defines it

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>

template <class T> class LQ {
    std::queue<T> q_;
    std::mutex m_;
    std::condition_variable cv_;

public:
    void push(T v) {
        { std::lock_guard<std::mutex> l(m_); q_.push(std::move(v)); }
        cv_.notify_one();
    }
    void pop(T& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return !q_.empty(); });
        out = std::move(q_.front());
        q_.pop();
    }
#ifdef OUT_OF_LINE
    std::size_t pending();
#endif
};

#ifndef NO_MAIN
int main() {
    LQ<int> q;
    q.push(1);
    int v;
    q.pop(v);
#ifdef OUT_OF_LINE
    return static_cast<int>(q.pending()) + v;
#else
    return v;
#endif
}
#else
void use(LQ<int>& q) {
    q.push(1);
    int v;
    q.pop(v);
}
#endif
