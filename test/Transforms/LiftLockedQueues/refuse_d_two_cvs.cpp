// (d) The pops wait on one condition variable, and the waiting pushes of a
// bounded ring on one: the substitute wakes one set of waiters for an
// element and one for room. Refused: two pops that wait on two variables
// (TwoForData), and two pushes that wait for room on two (TwoForRoom).
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in TwoForData not lifted: (d) the pops wait on two condition variables
// CHECK-DAG: locked queue q_ in TwoForRoom not lifted: (d) the pushes wait on two condition variables
// CHECK-NOT: lifts

#include "Inputs/circular_q.h"
#include <condition_variable>
#include <mutex>
#include <queue>

class TwoForData {
    std::queue<int> q_;
    std::mutex m_;
    std::condition_variable a_, b_;

public:
    void push(int v) {
        {
            std::lock_guard<std::mutex> l(m_);
            q_.push(v);
        }
        a_.notify_one();
        b_.notify_one();
    }
    void pop(int &out) {
        std::unique_lock<std::mutex> l(m_);
        a_.wait(l, [this] { return !q_.empty(); });
        out = q_.front();
        q_.pop();
    }
    void pop_other(int &out) {
        std::unique_lock<std::mutex> l(m_);
        b_.wait(l, [this] { return !q_.empty(); });
        out = q_.front();
        q_.pop();
    }
};

class TwoForRoom {
public:
    explicit TwoForRoom(size_t n) : q_(n) {}
    void push(int &&v) {
        {
            std::unique_lock<std::mutex> l(m_);
            space_.wait(l, [this] { return !q_.full(); });
            q_.push_back(std::move(v));
        }
        data_.notify_one();
    }
    void push_other(int &&v) {
        {
            std::unique_lock<std::mutex> l(m_);
            room_.wait(l, [this] { return !q_.full(); });
            q_.push_back(std::move(v));
        }
        data_.notify_one();
    }
    void pop(int &out) {
        {
            std::unique_lock<std::mutex> l(m_);
            data_.wait(l, [this] { return !q_.empty(); });
            out = std::move(q_.front());
            q_.pop_front();
        }
        space_.notify_one();
        room_.notify_one();
    }

private:
    std::mutex m_;
    std::condition_variable data_, space_, room_;
    Ring<int> q_;
};

int main() {
    int v;
    TwoForData d;
    d.push(1);
    d.pop(v);
    d.push(2);
    d.pop_other(v);
    TwoForRoom r(4);
    r.push(1);
    r.push_other(2);
    r.pop(v);
}
