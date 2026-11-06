// A finding of the review of 6 Nov 2026: one file, nothing linked: a
// pointer to the container member, from a static member function, through
// which main reads the container. Lifted, the program printed otherwise;
// now the class is refused, and the program prints what it prints as
// written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in SQ not lifted: (a) main makes a value that may point to it a pointer to std::queue<int, std::deque<int, std::allocator<int>>>, and reads SQ as that type

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
class SQ {
    std::queue<int> q_;
    std::mutex m_;
    std::condition_variable cv_;
public:
    void push(int v) {
        { std::lock_guard<std::mutex> l(m_); q_.push(v); }
        cv_.notify_one();
    }
    void pop(int &out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return !q_.empty(); });
        out = q_.front();
        q_.pop();
    }
    static std::queue<int> SQ::*container() { return &SQ::q_; }
};
int main() {
    SQ q;
    for (int i = 0; i < 3; ++i) q.push(i);
    std::printf("pending %zu\n", (q.*SQ::container()).size());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
