// (b) The container is touched without the mutex held, or one operation
// lets the mutex go between two touches, so that what it saw is no longer
// so when it acts.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Unlocked not lifted: (b) Unlocked::empty_now calls empty on q_ without the mutex held
// CHECK-DAG: locked queue q_ in Split not lifted: (b) Split::try_pop lets the mutex go between two touches of q_
// CHECK-NOT: lifts

#include <deque>
#include <mutex>

class Unlocked {
public:
    void push(int v) { std::lock_guard<std::mutex> l(m_); q_.push_back(v); }
    bool empty_now() { return q_.empty(); }
    bool try_pop(int& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
private:
    std::deque<int> q_;
    std::mutex m_;
};

class Split {
public:
    void push(int v) { std::lock_guard<std::mutex> l(m_); q_.push_back(v); }
    bool try_pop(int& out) {
        {
            std::lock_guard<std::mutex> l(m_);
            if (q_.empty()) return false;
        }
        std::lock_guard<std::mutex> l(m_);
        out = q_.front();
        q_.pop_front();
        return true;
    }
private:
    std::deque<int> q_;
    std::mutex m_;
};

int main() {
    int v;
    Unlocked u; u.push(1); u.empty_now(); u.try_pop(v);
    Split s; s.push(1); s.try_pop(v);
}
