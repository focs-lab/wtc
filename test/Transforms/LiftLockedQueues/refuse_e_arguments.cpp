// (e) The element pushed must be of the container's own type, built from
// one argument of that type: the substitute's push takes the element
// itself. Refused: an emplace from two arguments (Pairs); two pushes of
// two types, an int emplaced into a std::queue of long beside a push of
// a long (Mixed); and, on a bounded ring whose push_back is a template,
// a push where there is room of an int beside a waiting push of a long
// (Offer).
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Pairs not lifted: (e) Pairs::push constructs the element in place from 2 arguments
// CHECK-DAG: locked queue q_ in Mixed not lifted: (e) Mixed::push_int constructs the element from another type
// CHECK-DAG: locked queue q_ in Offer not lifted: (e) Offer::offer constructs the element from another type
// CHECK-NOT: lifts

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

#define TRY_POP(T)                             \
    bool try_pop(T &out) {                     \
        std::lock_guard<std::mutex> l(m_);     \
        if (q_.empty()) return false;          \
        out = q_.front();                      \
        q_.pop();                              \
        return true;                           \
    }

using IntPair = std::pair<int, int>;
class Pairs {
    std::queue<IntPair> q_;
    std::mutex m_;

public:
    void push(int a, int b) {
        std::lock_guard<std::mutex> l(m_);
        q_.emplace(a, b);
    }
    TRY_POP(IntPair)
};

class Mixed {
    std::queue<long> q_;
    std::mutex m_;

public:
    void push(long v) {
        std::lock_guard<std::mutex> l(m_);
        q_.push(v);
    }
    void push_int(int v) {
        std::lock_guard<std::mutex> l(m_);
        q_.emplace(v);
    }
    TRY_POP(long)
};

// spdlog's circular_q (Inputs/circular_q.h) with push_back a template.
template <typename T>
class AnyRing {
    size_t max_items_ = 0;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t overrun_counter_ = 0;
    std::vector<T> v_;

public:
    using value_type = T;
    AnyRing() = default;
    explicit AnyRing(size_t max_items) : max_items_(max_items + 1), v_(max_items_) {}
    template <class U>
    void push_back(U &&item) {
        if (max_items_ > 0) {
            v_[tail_] = std::move(item);
            tail_ = (tail_ + 1) % max_items_;
            if (tail_ == head_) {
                head_ = (head_ + 1) % max_items_;
                ++overrun_counter_;
            }
        }
    }
    T &front() { return v_[head_]; }
    size_t size() const {
        if (tail_ >= head_) return tail_ - head_;
        return max_items_ - (head_ - tail_);
    }
    void pop_front() { head_ = (head_ + 1) % max_items_; }
    bool empty() const { return tail_ == head_; }
    bool full() const {
        if (max_items_ > 0) return ((tail_ + 1) % max_items_) == head_;
        return false;
    }
    size_t overrun_counter() const { return overrun_counter_; }
};

class Offer {
public:
    explicit Offer(size_t n) : q_(n) {}
    void push(long &&v) {
        {
            std::unique_lock<std::mutex> l(m_);
            space_.wait(l, [this] { return !q_.full(); });
            q_.push_back(std::move(v));
        }
        data_.notify_one();
    }
    void offer(int &&v) {
        bool pushed = false;
        {
            std::unique_lock<std::mutex> l(m_);
            if (!q_.full()) {
                q_.push_back(std::move(v));
                pushed = true;
            }
        }
        if (pushed) data_.notify_one();
    }
    void pop(long &out) {
        {
            std::unique_lock<std::mutex> l(m_);
            data_.wait(l, [this] { return !q_.empty(); });
            out = std::move(q_.front());
            q_.pop_front();
        }
        space_.notify_one();
    }

private:
    std::mutex m_;
    std::condition_variable data_, space_;
    AnyRing<long> q_;
};

int main() {
    long w;
    IntPair p;
    Pairs a;
    a.push(1, 2);
    a.try_pop(p);
    Mixed b;
    b.push(1);
    b.push_int(2);
    b.try_pop(w);
    Offer c(4);
    c.push(1);
    c.offer(2);
    c.pop(w);
}
