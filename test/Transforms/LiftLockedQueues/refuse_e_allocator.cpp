// (e) A std::deque or std::list lifts only with std::allocator of its
// element. An allocator of the program's own may bound the container (it
// throws once it has handed out so much, and the program counts the pushes
// refused) or count what it allocates; the substitute allocates with
// neither, so a class whose container, or whose std::queue's underlying
// container, takes another allocator is refused. The same class with the
// default allocator lifts.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Q<std::deque<long, Capped<long>>> not lifted: (e) the container allocates with an allocator other than std::allocator of its element (std::deque<long, Capped<long>>)
// CHECK-DAG: locked queue q_ in Q<std::__cxx11::list<long, Capped<long>>> not lifted: (e) the container allocates with an allocator other than std::allocator of its element (std::__cxx11::list<long, Capped<long>>)
// CHECK-DAG: locked queue q_ in Q<std::queue<long, std::deque<long, Capped<long>>>> not lifted: (e) the container allocates with an allocator other than std::allocator of its element (std::deque<long, Capped<long>>)
// CHECK-DAG: locked queue q_ in Q<std::deque<long, std::allocator<long>>> lifts
// CHECK-DAG: locked queue q_ in Q<std::queue<long, std::deque<long, std::allocator<long>>>> lifts

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <queue>

static int blocks = 0;
template <class T> struct Capped {
    using value_type = T;
    Capped() = default;
    template <class U> Capped(const Capped<U>&) {}
    T* allocate(std::size_t n) {
        if (++blocks > 3) throw std::bad_alloc();
        return std::allocator<T>().allocate(n);
    }
    void deallocate(T* p, std::size_t n) {
        --blocks;
        std::allocator<T>().deallocate(p, n);
    }
    template <class U> bool operator==(const Capped<U>&) const { return true; }
    template <class U> bool operator!=(const Capped<U>&) const { return false; }
};

template <class C> class Q {
    C q_;
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

// std::queue pushes with push and pops with pop.
template <class T, class C> class Q<std::queue<T, C>> {
    std::queue<T, C> q_;
    std::mutex m_;
    std::condition_variable cv_;

public:
    void push(long v) {
        { std::lock_guard<std::mutex> l(m_); q_.push(v); }
        cv_.notify_one();
    }
    bool try_pop(long& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop();
        return true;
    }
};

template <class C> long run() {
    Q<C> q;
    for (long i = 0; i < 4; ++i) q.push(i);
    long v, s = 0;
    while (q.try_pop(v)) s += v;
    return s;
}

int main() {
    return static_cast<int>(run<std::deque<long, Capped<long>>>() + run<std::list<long, Capped<long>>>() +
                            run<std::queue<long, std::deque<long, Capped<long>>>>() + run<std::deque<long>>() +
                            run<std::queue<long>>());
}
