// A value set holds each value as the integer its type reads, and an
// integral conversion is computed exactly. Each variant stores a value
// that makes the overrunning push run; read as a signed byte (UCHAR: 200
// stored in an unsigned char), compared signed (UNSIGNED: 4000000000 > 10
// in an unsigned), converted as if the conversion kept it (NARROW: 256
// made an unsigned char is 0) or read through one member of a union while
// another is written (UNION), the condition was decided false, the push
// left out, and the lifted program aborted when it ran. Each now keeps
// the overrun. Where the exact values do decide the condition (200 stored
// in an unsigned char compared with 100, DECIDED_UCHAR; 256 made an
// unsigned char compared with 1, DECIDED_NARROW), the push is still left
// out.
//
// RUN: %clangxx -std=c++17 -O1 -fclangir -emit-cir %s -DUCHAR -o %t.uchar.cir
// RUN: wtc-opt %t.uchar.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=KEPT
// RUN: %clangxx -std=c++17 -O1 -fclangir -emit-cir %s -DUNSIGNED -o %t.unsigned.cir
// RUN: wtc-opt %t.unsigned.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=KEPT
// RUN: %clangxx -std=c++17 -O1 -fclangir -emit-cir %s -DNARROW -o %t.narrow.cir
// RUN: wtc-opt %t.narrow.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=KEPT
// RUN: %clangxx -std=c++17 -O1 -fclangir -emit-cir %s -DUNION -o %t.union.cir
// RUN: wtc-opt %t.union.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefixes=KEPT,UNION
// RUN: %clangxx -std=c++17 -O1 -fclangir -emit-cir %s -DDECIDED_UCHAR -o %t.decided1.cir
// RUN: wtc-opt %t.decided1.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PRUNED
// RUN: %clangxx -std=c++17 -O1 -fclangir -emit-cir %s -DDECIDED_NARROW -o %t.decided2.cir
// RUN: wtc-opt %t.decided2.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PRUNED
//
// KEPT: Q::enqueue_nowait may overrun, so its callers are consumers too
// UNION-SAME: a member of Cfg, whose storage is shared with the other members of a union that holds it
// KEPT: locked queue q_ in Q lifts
// PRUNED: Q::enqueue_nowait never runs, so its overrun is left out
// PRUNED: locked queue q_ in Q lifts

#include "Inputs/circular_q.h"

#include <condition_variable>
#include <mutex>

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

#if defined(UCHAR) || defined(DECIDED_UCHAR)
struct Cfg {
    unsigned char mode;
};
#elif defined(UNSIGNED)
struct Cfg {
    unsigned mode;
};
#elif defined(NARROW) || defined(DECIDED_NARROW)
struct Cfg {
    int mode;
};
#elif defined(UNION)
struct Cfg {
    int mode;
};
struct Other {
    int raw;
};
union Both {
    Cfg c;
    Other o;
};
#endif

int main(int argc, char**) {
#if defined(UCHAR)
    Cfg c;
    c.mode = 200;
    const bool overrun = c.mode == 200;
#elif defined(UNSIGNED)
    Cfg c;
    c.mode = 4000000000u;
    const bool overrun = c.mode > 10u;
#elif defined(NARROW)
    Cfg c;
    c.mode = 256;
    const bool overrun = static_cast<unsigned char>(c.mode) == 0;
#elif defined(UNION)
    Both b;
    b.c.mode = 1;
    b.o.raw = argc - 1;  // 0 when run without arguments
    const bool overrun = b.c.mode == 0;
#elif defined(DECIDED_UCHAR)
    Cfg c;
    c.mode = 200;
    const bool overrun = c.mode == 100;
#elif defined(DECIDED_NARROW)
    Cfg c;
    c.mode = 256;
    const bool overrun = static_cast<unsigned char>(c.mode) == 1;
#endif
    Q q(2);
    for (int i = 0; i < 4; ++i) {
        if (overrun) q.enqueue_nowait(int(i));
        else q.enqueue(int(i));
    }
    int v, s = 0;
    for (int i = 0; i < 2; ++i) {
        q.dequeue(v);
        s += v;
    }
    return s;
}
