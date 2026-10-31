// A queue bounded by its container's allocator: the allocator throws once
// it has handed out three blocks, and the program counts the pushes that
// throw. The substitute allocates with neither the allocator nor its
// bound, so the class is refused (e), and the program, built as written,
// refuses the same pushes as the original. Lifted, it refused none.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc -O1 --no-lift %s -o %t.orig.exe
// RUN: %t.orig.exe | FileCheck %s --check-prefix=OUT
//
// CHECK: locked queue q_ in Q not lifted: (e) the container allocates with an allocator other than std::allocator of its element (std::deque<long, CapAlloc<long>>)
// CHECK-NOT: lifted (
// OUT: pushed 127 refused 873 popped 127

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <new>

static int blocks = 0;
template <class T> struct CapAlloc {
    using value_type = T;
    CapAlloc() = default;
    template <class U> CapAlloc(const CapAlloc<U>&) {}
    T* allocate(std::size_t n) {
        if (++blocks > 3) throw std::bad_alloc();
        return std::allocator<T>().allocate(n);
    }
    void deallocate(T* p, std::size_t n) {
        --blocks;
        std::allocator<T>().deallocate(p, n);
    }
    template <class U> bool operator==(const CapAlloc<U>&) const { return true; }
    template <class U> bool operator!=(const CapAlloc<U>&) const { return false; }
};

class Q {
    std::deque<long, CapAlloc<long>> q_;
    std::mutex m_;
    std::condition_variable cv_;

public:
    void push(long v) {
        { std::lock_guard<std::mutex> l(m_); q_.push_back(v); }
        cv_.notify_one();
    }
    bool try_pop(long& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }
};

int main() {
    Q q;
    int refused = 0, pushed = 0;
    for (long i = 0; i < 1000; ++i) {
        try {
            q.push(i);
            ++pushed;
        } catch (const std::bad_alloc&) {
            ++refused;
        }
    }
    long v, n = 0;
    while (q.try_pop(v)) ++n;
    std::printf("pushed %d refused %d popped %ld\n", pushed, refused, n);
}
