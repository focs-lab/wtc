// (c) The class's mutex guards the queue and nothing else: a class with a
// second mutex is refused, since the substitute has no mutex to keep for
// whatever the other guards beside the queue.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: locked queue q_ in TwoMutexes not lifted: (c) the class has two mutexes, m_ and n_
// CHECK-NOT: lifts

#include <mutex>
#include <queue>

class TwoMutexes {
    std::queue<int> q_;
    std::mutex m_;
    std::mutex n_;

public:
    void push(int v) {
        std::lock_guard<std::mutex> l(m_);
        q_.push(v);
    }
    bool try_pop(int &out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop();
        return true;
    }
};

int main() {
    int v;
    TwoMutexes q;
    q.push(1);
    q.try_pop(v);
}
