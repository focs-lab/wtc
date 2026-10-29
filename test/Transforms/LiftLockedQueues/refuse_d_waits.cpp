// (d) Waits the substitute could not keep: a time-out, a wait outside a
// predicate loop, a push that does not notify the variable a pop waits
// on, a pop that returns at the stop flag with elements still queued, a
// close that wakes one waiter, a condition_variable_any, and a pop that
// ignores the stop flag of a class that closes, which would return where
// the original waits on.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Timed not lifted: (d) Timed::pop waits with a time-out (wait_for): the time-out path has no faithful equivalent yet
// CHECK-DAG: locked queue q_ in IfWait not lifted: (d) IfWait::pop waits outside a predicate loop
// CHECK-DAG: locked queue q_ in Silent not lifted: (d) Silent::push does not notify cv_, which a pop waits on
// CHECK-DAG: locked queue q_ in Discard not lifted: (d) Discard::pop returns at the stop flag with elements still queued: discarding on stop has no faithful equivalent yet
// CHECK-DAG: locked queue q_ in OneWake not lifted: (d) OneWake::close sets the stop flag but does not wake every waiter on cv_
// CHECK-DAG: locked queue q_ in Any not lifted: (d) cv_ is a condition_variable_any
// CHECK-DAG: locked queue q_ in Deaf not lifted: (d) Deaf::pop waits only for an element in a class that also closes
// CHECK-NOT: lifts

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

#define PUSH                                                    \
    void push(int v) {                                          \
        { std::lock_guard<std::mutex> l(m_); q_.push_back(v); } \
        cv_.notify_one();                                       \
    }
#define MEMBERS            \
    std::deque<int> q_;    \
    std::mutex m_;         \
    std::condition_variable cv_;

class Timed {
public:
    PUSH
    bool pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        if (!cv_.wait_for(l, std::chrono::milliseconds(5), [this] { return !q_.empty(); })) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
private:
    MEMBERS
};

class IfWait {
public:
    PUSH
    void pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        if (q_.empty()) cv_.wait(l);
        out = q_.front();
        q_.pop_front();
    }
private:
    MEMBERS
};

class Silent {
public:
    void push(int v) { std::lock_guard<std::mutex> l(m_); q_.push_back(v); }
    void pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !q_.empty(); });
        out = q_.front();
        q_.pop_front();
    }
private:
    MEMBERS
};

class Discard {
public:
    PUSH
    bool pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !q_.empty() || stop_; });
        if (stop_) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
    void close() { { std::lock_guard<std::mutex> l(m_); stop_ = true; } cv_.notify_all(); }
private:
    MEMBERS
    bool stop_ = false;
};

class OneWake {
public:
    PUSH
    bool pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !q_.empty() || stop_; });
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
    void close() { { std::lock_guard<std::mutex> l(m_); stop_ = true; } cv_.notify_one(); }
private:
    MEMBERS
    bool stop_ = false;
};

class Any {
public:
    void push(int v) { { std::lock_guard<std::mutex> l(m_); q_.push_back(v); } cv_.notify_one(); }
    void pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !q_.empty(); });
        out = q_.front();
        q_.pop_front();
    }
private:
    std::deque<int> q_;
    std::mutex m_;
    std::condition_variable_any cv_;
};

class Deaf {
public:
    PUSH
    void pop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !q_.empty(); });
        out = q_.front();
        q_.pop_front();
    }
    bool pop_or_stop(int& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !q_.empty() || stop_; });
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
    void close() { { std::lock_guard<std::mutex> l(m_); stop_ = true; } cv_.notify_all(); }
private:
    MEMBERS
    bool stop_ = false;
};

int main() {
    int v;
    Timed a; a.push(1); a.pop(v);
    IfWait b; b.push(1); b.pop(v);
    Silent c; c.push(1); c.pop(v);
    Discard d; d.push(1); d.pop(v); d.close();
    OneWake e; e.push(1); e.pop(v); e.close();
    Any f; f.push(1); f.pop(v);
    Deaf g; g.push(1); g.pop(v); g.pop_or_stop(v); g.close();
}
