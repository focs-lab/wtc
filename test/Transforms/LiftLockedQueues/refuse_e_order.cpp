// (e) Orders and operations other than FIFO at the two ends: taking from
// the back, inserting at the front, a drain by swap, a count, and
// containers of other kinds. Each is counted in the "uses" remark.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Stack not lifted: (e) Stack::pop takes from the back (back): LIFO, not FIFO
// CHECK-DAG: locked queue q_ in FrontIn not lifted: (e) FrontIn::push_urgent inserts at the front (push_front)
// CHECK-DAG: locked queue q_ in Swap not lifted: (e) Swap::drain drains the container by swap
// CHECK-DAG: locked queue q_ in Count not lifted: (d) Count::size reads size(): the substitutes keep no linearisable count
// CHECK-DAG: locked queue q_ in Vec not lifted: (e) the container is a std::vector, which the class may use as a stack or a batch
// CHECK-DAG: locked queue q_ in Prio not lifted: (e) the container is a std::priority_queue: priority order, not FIFO
// CHECK-DAG: locked queue q_ in Swap uses empty 1, front 1, pop_front 1, push_back 1, swap 1
// CHECK-NOT: lifts

#include <deque>
#include <mutex>
#include <queue>
#include <vector>

#define PUSH(op) void push(int v) { std::lock_guard<std::mutex> l(m_); q_.op(v); }
#define TRY_POP                                 \
    bool try_pop(int& out) {                    \
        std::lock_guard<std::mutex> l(m_);      \
        if (q_.empty()) return false;           \
        out = q_.front();                       \
        q_.pop_front();                         \
        return true;                            \
    }

class Stack {
public:
    PUSH(push_back)
    bool pop(int& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.back();
        q_.pop_back();
        return true;
    }
private:
    std::deque<int> q_;
    std::mutex m_;
};

class FrontIn {
public:
    PUSH(push_back)
    TRY_POP
    void push_urgent(int v) { std::lock_guard<std::mutex> l(m_); q_.push_front(v); }
private:
    std::deque<int> q_;
    std::mutex m_;
};

class Swap {
public:
    PUSH(push_back)
    TRY_POP
    void drain(std::deque<int>& out) { std::lock_guard<std::mutex> l(m_); q_.swap(out); }
private:
    std::deque<int> q_;
    std::mutex m_;
};

class Count {
public:
    PUSH(push_back)
    TRY_POP
    size_t size() { std::lock_guard<std::mutex> l(m_); return q_.size(); }
private:
    std::deque<int> q_;
    std::mutex m_;
};

class Vec {
public:
    PUSH(push_back)
    bool try_pop(int& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.erase(q_.begin());
        return true;
    }
private:
    std::vector<int> q_;
    std::mutex m_;
};

class Prio {
public:
    PUSH(push)
    bool try_pop(int& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.top();
        q_.pop();
        return true;
    }
private:
    std::priority_queue<int> q_;
    std::mutex m_;
};

int main() {
    int v;
    std::deque<int> d;
    Stack a; a.push(1); a.pop(v);
    FrontIn b; b.push(1); b.push_urgent(2); b.try_pop(v);
    Swap c; c.push(1); c.drain(d); c.try_pop(v);
    Count e; e.push(1); e.size(); e.try_pop(v);
    Vec f; f.push(1); f.try_pop(v);
    Prio g; g.push(1); g.try_pop(v);
}
