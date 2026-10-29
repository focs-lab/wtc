// The shapes the pass accepts, each in a class of its own, reported with
// rewrite=false (no runtime in the module): a while loop or wait(lock,
// predicate) for a blocking pop; a stop flag in either order in the
// predicate, as a loop condition, and tested after the wait as "empty" or
// as "stopped and empty"; an element copied out where it is a scalar; a
// std::list or a std::deque as well as a std::queue; a scalar pushed by
// value; a push that returns true.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Loop lifts
// CHECK-DAG: locked queue q_ in Pred lifts
// CHECK-DAG: locked queue q_ in Closable lifts
// CHECK-DAG: locked queue q_ in ClosableLoop lifts
// CHECK-DAG: locked queue q_ in Scalar lifts
// CHECK-NOT: not lifted

#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <queue>
#include <string>

class Loop {
public:
    void push(std::string v) {
        { std::lock_guard<std::mutex> l(m_); q_.push(std::move(v)); }
        cv_.notify_one();
    }
    void pop(std::string& out) {
        std::unique_lock<std::mutex> l(m_);
        while (q_.empty()) cv_.wait(l);
        out = std::move(q_.front());
        q_.pop();
    }
private:
    std::queue<std::string> q_;
    std::mutex m_;
    std::condition_variable cv_;
};

class Pred {
public:
    void push(std::string&& v) {
        std::lock_guard<std::mutex> l(m_);
        q_.emplace_back(std::move(v));
        cv_.notify_all();
    }
    void pop(std::string& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !q_.empty(); });
        out = std::move(q_.front());
        q_.pop_front();
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::list<std::string> q_;
};

class Closable {
public:
    void push(std::string v) {
        { std::lock_guard<std::mutex> l(m_); q_.push_back(std::move(v)); }
        cv_.notify_one();
    }
    bool pop(std::string& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return stop_ || !q_.empty(); });
        if (stop_ && q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void close() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
    }
private:
    std::deque<std::string> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

class ClosableLoop {
public:
    void push(const std::string& v) {
        std::lock_guard<std::mutex> l(m_);
        q_.push_back(v);
        cv_.notify_one();
    }
    bool pop(std::string& out) {
        std::unique_lock<std::mutex> l(m_);
        while (q_.empty() && !stop_) cv_.wait(l);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void close() {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
        cv_.notify_all();
    }
private:
    std::deque<std::string> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

class Scalar {
public:
    bool push(long v) {
        std::lock_guard<std::mutex> l(m_);
        q_.push(v);
        return true;
    }
    bool try_pop(long& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop();
        return true;
    }
private:
    std::queue<long> q_;
    std::mutex m_;
};

int main() {
    std::string s;
    Loop a; a.push("x"); a.pop(s);
    Pred b; b.push("x"); b.pop(s);
    Closable c; c.push("x"); c.pop(s); c.close();
    ClosableLoop d; d.push(s); d.pop(s); d.close();
    Scalar e; long v; e.push(1); e.try_pop(v);
}
