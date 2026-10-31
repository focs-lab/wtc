// A locked queue in a header, used by two files.
#pragma once
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>

template <class T> class LQ {
    std::queue<T> q_;
    std::mutex m_;
    std::condition_variable cv_;

public:
    void push(T v) {
        { std::lock_guard<std::mutex> l(m_); q_.push(std::move(v)); }
        cv_.notify_one();
    }
    void pop(T& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return !q_.empty(); });
        out = std::move(q_.front());
        q_.pop();
    }
    std::size_t pending() {
        std::lock_guard<std::mutex> l(m_);
        return q_.size();
    }
};
