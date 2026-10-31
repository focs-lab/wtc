// Whether a push that overruns can run is decided from the values the
// condition around its call may hold; a value read from a member is every
// value stored into that member. That holds only while nothing reaches an
// object's storage other than member by member. Each variant below
// writes the policy member another way, and the overrunning push must be
// kept (it may overrun): an object value-initialised to zero, bytes
// copied over it, a pointer to member, a function with no body here that
// takes it, raw zeroed memory taken for the object, and a function of
// this file that another file calls with another policy. Stored member by
// member only, the condition is decided and the push left out.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -DPLAIN -o %t.plain.cir
// RUN: wtc-opt %t.plain.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PRUNED
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -DVALUEINIT -o %t.valueinit.cir
// RUN: wtc-opt %t.valueinit.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefixes=KEPT,VALUEINIT
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -DMEMCPY -o %t.memcpy.cir
// RUN: wtc-opt %t.memcpy.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefixes=KEPT,MEMCPY
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -DPTRMEM -o %t.ptrmem.cir
// RUN: wtc-opt %t.ptrmem.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefixes=KEPT,PTRMEM
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -DEXTERN -o %t.extern.cir
// RUN: wtc-opt %t.extern.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefixes=KEPT,EXTERN
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -DCALLOC -o %t.calloc.cir
// RUN: wtc-opt %t.calloc.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefixes=KEPT,CALLOC
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -DCALLED_ELSEWHERE -o %t.called.cir
// RUN: wtc-opt %t.called.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PRUNED
// RUN: echo _Z4postPvii > %t.names
// RUN: wtc-opt %t.called.cir --wtc-lift-locked-queues="rewrite=false named-elsewhere=%t.names" -o /dev/null 2>&1 | FileCheck %s --check-prefixes=KEPT,CALLED
//
// PRUNED: Q::enqueue_nowait never runs, so its overrun is left out
// PRUNED: locked queue q_ in Q lifts
// KEPT: Q::enqueue_nowait may overrun, so its callers are consumers too
// VALUEINIT-SAME: a member of Cfg, whose storage is made from a pointer of another type
// MEMCPY-SAME: a member of Cfg, whose storage is reached through a pointer of another type
// PTRMEM-SAME: a member of Cfg, whose storage is reached through a pointer of another type
// EXTERN-SAME: a member of Cfg, whose storage is handed to configure(Cfg&), which has no body here
// CALLOC-SAME: a member of Cfg, whose storage is made from a pointer of another type
// CALLED-SAME: a parameter of post(void*, int, int), which another file may call
// KEPT: locked queue q_ in Q lifts

#include "Inputs/circular_q.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

struct Cfg {
    int policy;
};

class Q {
    std::mutex m_;
    std::condition_variable data_, space_;
    Ring<int> q_;

public:
    explicit Q(std::size_t n) : q_(n) {}
    void enqueue(int&& v) {
        {
            std::unique_lock<std::mutex> l(m_);
            space_.wait(l, [this] { return !q_.full(); });
            q_.push_back(std::move(v));
        }
        data_.notify_one();
    }
    void enqueue_nowait(int&& v) {
        { std::unique_lock<std::mutex> l(m_); q_.push_back(std::move(v)); }
        data_.notify_one();
    }
    void dequeue(int& out) {
        {
            std::unique_lock<std::mutex> l(m_);
            data_.wait(l, [this] { return !q_.empty(); });
            out = std::move(q_.front());
            q_.pop_front();
        }
        space_.notify_one();
    }
};

#ifdef EXTERN
void configure(Cfg& c);
#endif

// The overflow policy's choice, as spdlog's thread pool makes it. The
// queue comes as void *, so that the function's name does not name Q (a
// file that names Q would keep it from lifting at all).
void post(void* to, int policy, int i) {
    Q& q = *static_cast<Q*>(to);
    if (policy == 0) q.enqueue_nowait(int(i));
    else q.enqueue(int(i));
}

int main(int argc, char**) {
#if defined(PLAIN)
    Cfg c;
    c.policy = 1;
    const int policy = c.policy;
#elif defined(VALUEINIT)
    Cfg* c = new Cfg();  // policy 0, by value-initialisation
    if (argc > 5) c->policy = 1;
    const int policy = c->policy;
#elif defined(MEMCPY)
    Cfg c;
    c.policy = 1;
    int z = argc - 1;
    std::memcpy(&c, &z, sizeof z);
    const int policy = c.policy;
#elif defined(PTRMEM)
    int Cfg::*pm = &Cfg::policy;
    Cfg c;
    c.policy = 1;
    c.*pm = argc - 1;
    const int policy = c.policy;
#elif defined(EXTERN)
    Cfg c;
    c.policy = 1;
    configure(c);  // defined in another file
    const int policy = c.policy;
#elif defined(CALLOC)
    Cfg* c = static_cast<Cfg*>(std::calloc(1, sizeof(Cfg)));  // policy 0
    if (argc > 5) c->policy = 1;
    const int policy = c->policy;
#elif defined(CALLED_ELSEWHERE)
    const int policy = 1;
#endif
    Q q(4);
    for (int i = 0; i < 4; ++i) post(&q, policy, i);
    int v, s = 0;
    for (int i = 0; i < 4; ++i) {
        q.dequeue(v);
        s += v;
    }
    return s;
}
