// (d) A notification made with the mutex held. Under the original's mutex
// a thread that sees what the operation did (takes the pushed element,
// finds the queue closed) sees it only once the notification has returned,
// and may then destroy the queue. The substitute wakes its sleepers after
// the element is visible, so that thread could destroy it while the
// wake-up still runs. A push or a close that notifies under the mutex
// refuses; one that notifies after releasing it, the usual form, lifts:
// there the original itself touches the condition variable after the
// point at which another thread may act.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in PushHeld not lifted: (d) PushHeld::push notifies cv_ with the mutex held: a thread that sees its effect may destroy the queue while the substitute is still waking a sleeper
// CHECK-DAG: locked queue q_ in CloseHeld not lifted: (d) CloseHeld::close notifies cv_ with the mutex held
// CHECK-DAG: locked queue q_ in After lifts

#include <condition_variable>
#include <deque>
#include <mutex>

#define POP                                                    \
    bool pop(int& out) {                                       \
        std::unique_lock<std::mutex> l(m_);                    \
        cv_.wait(l, [&] { return !q_.empty() || stop_; });     \
        if (q_.empty()) return false;                          \
        out = q_.front();                                      \
        q_.pop_front();                                        \
        return true;                                           \
    }
#define MEMBERS              \
    std::deque<int> q_;      \
    std::mutex m_;           \
    std::condition_variable cv_; \
    bool stop_ = false;

class PushHeld {
public:
    void push(int v) {
        std::lock_guard<std::mutex> l(m_);
        q_.push_back(v);
        cv_.notify_one();
    }
    POP
    void close() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
    }
private:
    MEMBERS
};

class CloseHeld {
public:
    void push(int v) {
        { std::lock_guard<std::mutex> l(m_); q_.push_back(v); }
        cv_.notify_one();
    }
    POP
    void close() {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
        cv_.notify_all();
    }
private:
    MEMBERS
};

class After {
public:
    void push(int v) {
        { std::lock_guard<std::mutex> l(m_); q_.push_back(v); }
        cv_.notify_one();
    }
    POP
    void close() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
    }
private:
    MEMBERS
};

template <class Q> int use() {
    Q q;
    int v = 0;
    q.push(1);
    q.pop(v);
    q.close();
    return v;
}

int main() { return use<PushHeld>() + use<CloseHeld>() + use<After>(); }
