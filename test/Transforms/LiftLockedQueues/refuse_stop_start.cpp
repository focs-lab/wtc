// The substitute starts open, so the stop flag the waits read must start
// false: set by a constant in every constructor. A flag that starts true
// (the original's closable pop returns false at once on an empty queue,
// the substitute's waits), one computed from an argument, one std::atomic
// left indeterminate by its default constructor, and a second constructor
// that starts it true refuse; false by a default member initialiser lifts.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in StartsTrue not lifted: (d) StartsTrue::StartsTrue starts the stop flag stop_ other than false: the substitute starts open
// CHECK-DAG: locked queue q_ in Computed not lifted: (d) Computed::Computed starts the stop flag stop_ other than false: the substitute starts open
// CHECK-DAG: locked queue q_ in Indeterminate not lifted: (d) Indeterminate::Indeterminate starts the stop flag stop_ indeterminate: the substitute starts open
// CHECK-DAG: locked queue q_ in TwoCtors not lifted: (d) TwoCtors::TwoCtors starts the stop flag stop_ other than false: the substitute starts open
// CHECK-DAG: locked queue q_ in StartsFalse lifts

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

#define BODY                                                         \
    void push(int v) {                                               \
        { std::lock_guard<std::mutex> l(m_); q_.push(v); }           \
        cv_.notify_one();                                            \
    }                                                                \
    bool pop(int& out) {                                             \
        std::unique_lock<std::mutex> l(m_);                          \
        cv_.wait(l, [&] { return !q_.empty() || stop_; });           \
        if (q_.empty()) return false;                                \
        out = q_.front();                                            \
        q_.pop();                                                    \
        return true;                                                 \
    }                                                                \
    void close() {                                                   \
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }         \
        cv_.notify_all();                                            \
    }
#define MEMBERS            \
    std::queue<int> q_;    \
    std::mutex m_;         \
    std::condition_variable cv_;

class StartsTrue {
public:
    BODY
private:
    MEMBERS
    bool stop_ = true;
};

class Computed {
public:
    explicit Computed(bool s) : stop_(s) {}
    BODY
private:
    MEMBERS
    bool stop_;
};

class Indeterminate {
public:
    Indeterminate() {}
    BODY
private:
    MEMBERS
    std::atomic<bool> stop_;
};

class TwoCtors {
public:
    TwoCtors() : stop_(false) {}
    explicit TwoCtors(int) : stop_(true) {}
    BODY
private:
    MEMBERS
    bool stop_;
};

class StartsFalse {
public:
    BODY
private:
    MEMBERS
    bool stop_ = false;
};

template <class Q> int use(Q& q) {
    int v = 0;
    q.push(1);
    q.pop(v);
    q.close();
    return v;
}

int main(int argc, char**) {
    StartsTrue a;
    Computed b(argc > 5);
    Indeterminate c;
    TwoCtors d, e(1);
    StartsFalse f;
    return use(a) + use(b) + use(c) + use(d) + use(e) + use(f);
}
