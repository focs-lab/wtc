// (e) std::queue is FIFO only over a container that is: the adapter's
// member must be a std::deque or a std::list itself. A container of the
// program's own whose push_back puts at the front (LIFO), and whose type
// name nevertheless carries ", std::deque<" in the adapter's spelling,
// refuses; the default and an explicit std::list lift.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Q<std::queue<int, Rev<int, std::deque<int, std::allocator<int>>>>> not lifted: (e) the std::queue's underlying container is neither a std::deque nor a std::list (Rev<int, std::deque<int, std::allocator<int>>>)
// CHECK-DAG: locked queue q_ in Q<std::queue<int, std::deque<int, std::allocator<int>>>> lifts
// CHECK-DAG: locked queue q_ in Q<std::queue<int, std::__cxx11::list<int, std::allocator<int>>>> lifts

#include <cstddef>
#include <deque>
#include <list>
#include <mutex>
#include <queue>

template <class T, class D = std::deque<T>> struct Rev {
    using value_type = T;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using container_type = D;
    D d;
    bool empty() const { return d.empty(); }
    size_type size() const { return d.size(); }
    T& front() { return d.front(); }
    const T& front() const { return d.front(); }
    T& back() { return d.back(); }
    const T& back() const { return d.back(); }
    void push_back(const T& x) { d.push_front(x); }
    void push_back(T&& x) { d.push_front(std::move(x)); }
    void pop_front() { d.pop_front(); }
};

template <class A> class Q {
    A q_;
    std::mutex m_;

public:
    void push(int v) {
        std::lock_guard<std::mutex> l(m_);
        q_.push(v);
    }
    bool try_pop(int& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop();
        return true;
    }
};

template <class A> int use() {
    Q<A> q;
    for (int i = 1; i <= 3; ++i) q.push(i);
    int v = 0, s = 0;
    while (q.try_pop(v)) s = s * 10 + v;
    return s;
}

int main() {
    return use<std::queue<int, Rev<int, std::deque<int>>>>() + use<std::queue<int>>() +
           use<std::queue<int, std::list<int>>>();
}
