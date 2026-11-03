// (e) emplace builds the element from its argument, which may be of
// another type: an int emplaced into a queue of long is converted where
// the container builds it. The substitute's push takes the element
// itself, so such a class refuses; emplacing the element's own type lifts.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Converting not lifted: (e) the element is built by emplace from an argument of another type than the container's
// CHECK-DAG: locked queue q_ in Same lifts

#include <mutex>
#include <queue>

#define TRY_POP                                \
    bool try_pop(long& out) {                  \
        std::lock_guard<std::mutex> l(m_);     \
        if (q_.empty()) return false;          \
        out = q_.front();                      \
        q_.pop();                              \
        return true;                           \
    }

class Converting {
    std::queue<long> q_;
    std::mutex m_;

public:
    void push(int v) {
        std::lock_guard<std::mutex> l(m_);
        q_.emplace(v);
    }
    TRY_POP
};

class Same {
    std::queue<long> q_;
    std::mutex m_;

public:
    void push(long v) {
        std::lock_guard<std::mutex> l(m_);
        q_.emplace(v);
    }
    TRY_POP
};

int main() {
    Converting c;
    Same s;
    long v = 0, t = 0;
    c.push(-3);
    s.push(4);
    c.try_pop(v);
    s.try_pop(t);
    return static_cast<int>(v + t);
}
