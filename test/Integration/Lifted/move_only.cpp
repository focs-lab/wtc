// A queue of std::unique_ptr, the common shape of a task queue: the
// element can be moved and not copied. The runtime is instantiated whole
// for the container type, and its copying push must still compile; the
// class lifts, builds and runs.
//
// RUN: %wtc_cc -DWTC_CHECK -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK: locked queue q_ in Q lifted (
// OUT: 5 6

#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <queue>

class Q {
    std::queue<std::unique_ptr<int>> q_;
    std::mutex m_;
    std::condition_variable cv_;

public:
    void push(std::unique_ptr<int> p) {
        { std::lock_guard<std::mutex> l(m_); q_.push(std::move(p)); }
        cv_.notify_one();
    }
    void pop(std::unique_ptr<int>& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return !q_.empty(); });
        out = std::move(q_.front());
        q_.pop();
    }
};

int main() {
    Q q;
    q.push(std::make_unique<int>(5));
    q.push(std::make_unique<int>(6));
    std::unique_ptr<int> a, b;
    q.pop(a);
    q.pop(b);
    std::printf("%d %d\n", *a, *b);
}
