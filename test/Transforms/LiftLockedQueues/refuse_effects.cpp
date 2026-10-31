// A lifted function's body is replaced whole, so whatever else it does
// must be nothing the program can see. A local object built or destroyed
// under the lock is accepted only when its type is known to do nothing
// else (a lock, a duration); a local of any other type, whose constructor
// or destructor may count, log or allocate, refuses the class. A lock is
// still fine.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Audited not lifted: (a) Audited::push builds or destroys a local Audit, whose constructor or destructor may have effects of its own
// CHECK-DAG: locked queue q_ in Plain lifts

#include <condition_variable>
#include <mutex>
#include <queue>

static int audits = 0;
struct Audit {
    Audit() { ++audits; }
};

#define POP                                           \
    void pop(int& out) {                              \
        std::unique_lock<std::mutex> l(m_);           \
        cv_.wait(l, [&] { return !q_.empty(); });     \
        out = q_.front();                             \
        q_.pop();                                     \
    }
#define MEMBERS            \
    std::queue<int> q_;    \
    std::mutex m_;         \
    std::condition_variable cv_;

class Audited {
public:
    void push(int v) {
        {
            std::lock_guard<std::mutex> l(m_);
            Audit a;
            q_.push(v);
        }
        cv_.notify_one();
    }
    POP
private:
    MEMBERS
};

class Plain {
public:
    void push(int v) {
        { std::unique_lock<std::mutex> l(m_); q_.push(v); }
        cv_.notify_one();
    }
    POP
private:
    MEMBERS
};

int main() {
    Audited a;
    Plain p;
    int v;
    a.push(1);
    a.pop(v);
    p.push(v);
    p.pop(v);
    return v + audits;
}
