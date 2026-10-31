// (c) The mutex guards something besides the queue, inside an operation or
// in a critical section of its own; splitting it would change what the
// program relies on. A stop flag set without the mutex is refused too: the
// original could lose that wake-up, and the substitute would not.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Counting not lifted: (c) the mutex m_ also guards pushed_ in Counting::push
// CHECK-DAG: locked queue q_ in Shared not lifted: (c) the mutex m_ also guards stat_ in Shared::bump
// CHECK-DAG: locked queue q_ in LooseStop not lifted: (c) the flag stop_ is set without the mutex in LooseStop::close
// CHECK-NOT: lifts

#include <condition_variable>
#include <deque>
#include <mutex>

class Counting {
public:
    void push(int v) { std::lock_guard<std::mutex> l(m_); q_.push_back(v); ++pushed_; }
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
    long pushed_ = 0;
};

class Shared {
public:
    void push(int v) { std::lock_guard<std::mutex> l(m_); q_.push_back(v); }
    void bump() { std::lock_guard<std::mutex> l(m_); ++stat_; }
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
    long stat_ = 0;
};

class LooseStop {
public:
    void push(int v) { { std::lock_guard<std::mutex> l(m_); q_.push_back(v); } cv_.notify_one(); }
    bool pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !q_.empty() || stop_; });
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
    void close() { stop_ = true; cv_.notify_all(); }
private:
    std::deque<int> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

int main() {
    int v;
    Counting c; c.push(1); c.try_pop(v);
    Shared s; s.push(1); s.bump(); s.try_pop(v);
    LooseStop t; t.push(1); t.pop(v); t.close();
}
