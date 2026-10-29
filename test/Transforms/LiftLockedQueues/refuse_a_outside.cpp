// (a) The container is touched by something other than the constructor,
// the destructor and the five operations: a member function of another
// shape, a function that is not a member, and a copy of the object.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Peek not lifted: (a) Peek::peek returns something other than a constant
// CHECK-DAG: locked queue q in Open not lifted: (a) drain touches the members of Open outside its member functions
// CHECK-DAG: locked queue q_ in Copied not lifted: (a) Copied::Copied calls {{.*}}deque{{.*}} on q_
// CHECK-NOT: lifts

#include <condition_variable>
#include <deque>
#include <mutex>

class Peek {
public:
    void push(int v) { std::lock_guard<std::mutex> l(m_); q_.push_back(v); }
    bool try_pop(int& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
    int peek() { std::lock_guard<std::mutex> l(m_); return q_.front(); }
private:
    std::deque<int> q_;
    std::mutex m_;
};

struct Open {
    void push(int v) { std::lock_guard<std::mutex> l(m); q.push_back(v); }
    bool try_pop(int& out) {
        std::lock_guard<std::mutex> l(m);
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
    std::deque<int> q;
    std::mutex m;
};
void drain(Open& o) {
    std::lock_guard<std::mutex> l(o.m);
    while (!o.q.empty()) o.q.pop_front();
}

class Copied {
public:
    Copied() = default;
    Copied(const Copied& other) : q_(other.q_) {}
    void push(int v) { std::lock_guard<std::mutex> l(m_); q_.push_back(v); }
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
};

int main() {
    int v;
    Peek p; p.push(1); p.try_pop(v); p.peek();
    Open o; o.push(1); o.try_pop(v); drain(o);
    Copied c; c.push(1); Copied d(c); d.push(2); d.try_pop(v);
}
