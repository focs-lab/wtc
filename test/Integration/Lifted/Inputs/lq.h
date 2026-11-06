// For the tests of the review of 23 Nov 2026: a locked queue of the shape the pass lifts (g3log's shared_queue): a
// std::queue under a std::mutex, a condition variable, push and a blocking
// pop. Standard-layout, so a pointer to it and a pointer to its first
// member, the std::queue, are pointer-interconvertible ([basic.compound]).
#pragma once
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>

struct LQ {
    std::queue<int> q_;
    std::mutex m_;
    std::condition_variable cv_;
#ifdef LQ_OWN_CTOR
    LQ() { LQ_OWN_CTOR }
#endif
    void push(int v) {
        { std::lock_guard<std::mutex> l(m_); q_.push(v); }
        cv_.notify_one();
    }
    void pop(int &out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return !q_.empty(); });
        out = q_.front();
        q_.pop();
    }
};
