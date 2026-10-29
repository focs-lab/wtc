// (e) A ring is read symbolically, its counts as unsigned 64-bit values,
// and an integral conversion is the identity only where it keeps every
// value of its type. Narrow wraps its tail through an unsigned char: with
// 301 slots the tail goes back to zero after 255, which the ring's
// canonical push does not, and the substitute, a ring of the constructor's
// capacity, popped elements the original had overwritten. Refused. The
// circular_q ring, whose constants are converted from int, lifts.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Q<RingN<int>> not lifted: (e) the ring's RingN<int>::push_back converts a value to a type that does not hold all of its values
// CHECK-DAG: locked queue q_ in Q<Ring<int>> lifts

#include "Inputs/circular_q.h"

#include <condition_variable>
#include <mutex>

template <typename T> class RingN {
    size_t max_items_ = 0, head_ = 0, tail_ = 0, overrun_counter_ = 0;
    std::vector<T> v_;

public:
    using value_type = T;
    RingN() = default;
    explicit RingN(size_t max_items) : max_items_(max_items + 1), v_(max_items_) {}
    void push_back(T&& item) {
        if (max_items_ > 0) {
            v_[tail_] = std::move(item);
            tail_ = static_cast<unsigned char>(tail_ + 1) % max_items_;
            if (tail_ == head_) {
                head_ = (head_ + 1) % max_items_;
                ++overrun_counter_;
            }
        }
    }
    T& front() { return v_[head_]; }
    void pop_front() { head_ = (head_ + 1) % max_items_; }
    bool empty() const { return tail_ == head_; }
    bool full() const {
        if (max_items_ > 0) return ((tail_ + 1) % max_items_) == head_;
        return false;
    }
};

template <class R> class Q {
    std::mutex m_;
    std::condition_variable data_;
    R q_;

public:
    explicit Q(size_t n) : q_(n) {}
    void put_nowait(int&& v) {
        { std::unique_lock<std::mutex> l(m_); q_.push_back(std::move(v)); }
        data_.notify_one();
    }
    bool try_pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
};

template <class R> long run() {
    Q<R> q(300);
    for (int i = 0; i < 302; ++i) q.put_nowait(int(i));
    int v;
    long s = 0;
    while (q.try_pop(v)) s += v;
    return s;
}

int main() { return static_cast<int>(run<RingN<int>>() + run<Ring<int>>()); }
