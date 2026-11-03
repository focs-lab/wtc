// The overrunning push runs in each variant, by a value the pass once
// read wrongly (test/Transforms/LiftLockedQueues/bounded_overrun_values.cpp):
// an unsigned char of 200 read as -56 (UCHAR), an unsigned compared signed
// (UNSIGNED), a narrowing taken for the identity (NARROW), a member written
// through another member of a union (UNION). The pass proved that the push
// never ran, built the queue without overruns, and the lifted program
// aborted when it ran. It now keeps the overrun, and the lifted program
// prints what the original prints.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DUCHAR -I%S/../../Transforms/LiftLockedQueues/Inputs %s -o %t.uchar.exe 2>&1 | FileCheck %s
// RUN: %t.uchar.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DUNSIGNED -I%S/../../Transforms/LiftLockedQueues/Inputs %s -o %t.unsigned.exe 2>&1 | FileCheck %s
// RUN: %t.unsigned.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DNARROW -I%S/../../Transforms/LiftLockedQueues/Inputs %s -o %t.narrow.exe 2>&1 | FileCheck %s
// RUN: %t.narrow.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DUNION -I%S/../../Transforms/LiftLockedQueues/Inputs %s -o %t.union.exe 2>&1 | FileCheck %s
// RUN: %t.union.exe | FileCheck %s --check-prefix=OUT
//
// CHECK: Q::enqueue_nowait may overrun, so its callers are consumers too
// CHECK: locked queue q_ in Q lifted (
// OUT: overrun 1 sum 5

#include "circular_q.h"

#include <condition_variable>
#include <cstdio>
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

#if defined(UCHAR)
struct Cfg {
    unsigned char mode;
};
#elif defined(UNSIGNED)
struct Cfg {
    unsigned mode;
};
#elif defined(NARROW)
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
    std::printf("overrun %d sum %d\n", (int)overrun, s);
}
