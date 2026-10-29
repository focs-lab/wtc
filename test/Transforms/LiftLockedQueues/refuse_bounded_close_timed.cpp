// (d) A ring class that closes, with a pop that waits with a time limit
// for an element only. The original's timed pop ignores the stop flag:
// after close() it still takes an element pushed before its time is up.
// The substitute's gives up at close(), so the class is refused, as a pop
// or a push that waits without a limit for an element or for room is.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: locked queue q_ in Q not lifted: (d) Q::pop_for waits with a time limit only for an element in a class that also closes
// CHECK-NOT: lifts

#include "Inputs/circular_q.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

class Q {
    std::mutex m_;
    std::condition_variable data_;
    bool stop_ = false;
    Ring<int> q_;

public:
    std::size_t discards_ = 0;
    explicit Q(std::size_t n) : q_(n) {}
    void offer(int&& v) {
        bool pushed = false;
        {
            std::unique_lock<std::mutex> l(m_);
            if (!q_.full()) {
                q_.push_back(std::move(v));
                pushed = true;
            }
        }
        if (pushed) data_.notify_one();
        else ++discards_;
    }
    bool pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        data_.wait(l, [this] { return !q_.empty() || stop_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    bool pop_for(int& out, std::chrono::milliseconds d) {
        std::unique_lock<std::mutex> l(m_);
        if (!data_.wait_for(l, d, [this] { return !q_.empty(); })) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void close() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        data_.notify_all();
    }
};

int main() {
    Q q(4);
    q.close();
    q.offer(5);
    int out = -1, v = 0;
    q.pop_for(out, std::chrono::milliseconds(20));
    return out + (q.pop(v) ? v : 0);
}
