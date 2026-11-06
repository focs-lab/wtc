// The critical sections of lock objects (CriticalSections.h): the body of
// the cleanup scope that destroys a lock_guard, unique_lock or scoped_lock,
// up to the lock's own unlock(). What the section reads and writes is a
// member path from `this` (arg0); a member function called inside adds
// what it itself touches through its parameters, one level down, so that
// pop_task, which takes no lock, is seen under the mutex its caller holds.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-critical-sections -o /dev/null 2>&1 | FileCheck %s

#include <mutex>
#include <queue>

struct P {
    std::queue<int> q;  // arg0.0
    std::mutex m;       // arg0.1
    int n = 0;          // arg0.2
    int k = 0;          // arg0.3

    int pop_task() {
        int v = q.front();
        q.pop();
        return v;
    }
    // CHECK-DAG: remark: wtc: critical section in P::worker() on arg0.1; reads arg0 arg0.0 arg0.2; writes arg0.2; calls P::pop_task()
    void worker() {
        std::unique_lock<std::mutex> l(m);
        n++;
        int v = pop_task();
        l.unlock();
        k = v;
    }
    // CHECK-DAG: remark: wtc: critical section in P::push(int) on arg0.1; reads arg0.0 arg0.0.0; writes ; calls std::queue<int, std::deque<int, std::allocator<int>>>::push(int const&)
    void push(int v) {
        {
            std::lock_guard<std::mutex> g(m);
            q.push(v);
        }
        n = 1;
    }
    // CHECK-DAG: remark: wtc: critical section in P::count() on arg0.1; reads arg0.3; writes ; calls{{$}}
    int count() {
        std::scoped_lock g(m);
        return k;
    }
};

int main() {
    P p;
    p.push(1);
    p.worker();
    return p.count();
}
