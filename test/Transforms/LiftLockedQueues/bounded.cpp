// Bounded rings, read from their member functions (LiftedBounded.cpp), and
// the classes that lock them: what lifts and why the rest does not. The
// ring is spdlog's circular_q, written out once in a macro and varied.
//
//   Bounded       waits for room, pushes if there is room and counts a
//                 discard otherwise, pops with and without a time limit,
//                 hands back its size and overrun count: lifts
//   CountDecides  the program decides on the overrun count: the count is
//                 exact only once the pushes have returned, so refused
//   NoOverrun     a ring whose push drops the new element when full
//                 instead of the oldest: not a ring the pass knows
//   OffByOne      a ring whose slots are its argument, not one more: its
//                 constructor is not the form the pass knows
//   Closing       a push that waits only for room, in a class that closes:
//                 the substitute's would give up at close()
//   Disabled      the ring built by its default constructor
//   Silent        a pop that does not wake the pushes waiting for room
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: locked queue q_ in Bounded lifts
// CHECK-DAG: locked queue q_ in Bounded uses
// CHECK-DAG: locked queue q_ in CountDecides not lifted: (d) the overrun count CountDecides::overruns returns is used for more than handing it back
// CHECK-DAG: locked queue q_ in NoOverrun not lifted: (e) the ring's RingNoOverrun<int>::push_back does something to the elements on one branch only
// CHECK-DAG: locked queue q_ in OffByOne not lifted: (e) the container RingOffByOne<int> is neither a standard container nor a bounded ring: its constructor does not set the slots to the capacity plus one
// CHECK-DAG: locked queue q_ in Closing not lifted: (d) Closing::push waits only for room in a class that also closes
// CHECK-DAG: locked queue q_ in Disabled not lifted: (a) Disabled::Disabled builds the ring other than with its capacity
// CHECK-DAG: locked queue q_ in Silent not lifted: (d) Silent::pop does not notify space_, which a push waits on for room
// CHECK-NOT: Bounded not lifted

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

// Ring: spdlog's circular_q. RingNoOverrun: a push that drops the new
// element when full. RingOffByOne: max_items slots.
#define RING(Name, SLOTS, GUARD)                                            \
    template <typename T>                                                   \
    class Name {                                                            \
        size_t max_items_ = 0;                                              \
        size_t head_ = 0;                                                   \
        size_t tail_ = 0;                                                   \
        size_t overrun_counter_ = 0;                                        \
        std::vector<T> v_;                                                  \
                                                                            \
    public:                                                                 \
        using value_type = T;                                               \
        Name() = default;                                                   \
        explicit Name(size_t max_items) : max_items_(SLOTS), v_(max_items_) {} \
        void push_back(T &&item) {                                          \
            if (max_items_ > 0) {                                           \
                GUARD                                                       \
                v_[tail_] = std::move(item);                                \
                tail_ = (tail_ + 1) % max_items_;                           \
                if (tail_ == head_) {                                       \
                    head_ = (head_ + 1) % max_items_;                       \
                    ++overrun_counter_;                                     \
                }                                                           \
            }                                                               \
        }                                                                   \
        T &front() { return v_[head_]; }                                    \
        size_t size() const {                                               \
            if (tail_ >= head_) return tail_ - head_;                       \
            return max_items_ - (head_ - tail_);                            \
        }                                                                   \
        void pop_front() { head_ = (head_ + 1) % max_items_; }              \
        bool empty() const { return tail_ == head_; }                       \
        bool full() const {                                                 \
            if (max_items_ > 0) return ((tail_ + 1) % max_items_) == head_; \
            return false;                                                   \
        }                                                                   \
        size_t overrun_counter() const { return overrun_counter_; }         \
    };

RING(Ring, max_items + 1, )
RING(RingNoOverrun, max_items + 1, if (((tail_ + 1) % max_items_) == head_) return;)
RING(RingOffByOne, max_items, )

#define POP(cv)                                                     \
    void pop(int &out) {                                            \
        {                                                           \
            std::unique_lock<std::mutex> l(m_);                     \
            data_.wait(l, [this] { return !q_.empty(); });          \
            out = std::move(q_.front());                            \
            q_.pop_front();                                         \
        }                                                           \
        cv.notify_one();                                            \
    }
#define PUSH_WAIT                                                   \
    void push(int &&v) {                                            \
        {                                                           \
            std::unique_lock<std::mutex> l(m_);                     \
            space_.wait(l, [this] { return !q_.full(); });          \
            q_.push_back(std::move(v));                             \
        }                                                           \
        data_.notify_one();                                         \
    }
#define MEMBERS(R)                                                  \
    std::mutex m_;                                                  \
    std::condition_variable data_, space_;                          \
    R q_;

class Bounded {
public:
    explicit Bounded(size_t n) : q_(n) {}
    PUSH_WAIT
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
        else ++discards_;
    }
    POP(space_)
    bool pop_for(int &out, std::chrono::milliseconds d) {
        {
            std::unique_lock<std::mutex> l(m_);
            if (!data_.wait_for(l, d, [this] { return !q_.empty(); })) return false;
            out = std::move(q_.front());
            q_.pop_front();
        }
        space_.notify_one();
        return true;
    }
    size_t size() {
        std::unique_lock<std::mutex> l(m_);
        return q_.size();
    }
    size_t overruns() {
        std::unique_lock<std::mutex> l(m_);
        return q_.overrun_counter();
    }
    size_t discards() const { return discards_; }

private:
    MEMBERS(Ring<int>)
    size_t discards_ = 0;
};

class CountDecides {
public:
    explicit CountDecides(size_t n) : q_(n) {}
    PUSH_WAIT
    POP(space_)
    size_t overruns() {
        std::unique_lock<std::mutex> l(m_);
        return q_.overrun_counter();
    }

private:
    MEMBERS(Ring<int>)
};

class NoOverrun {
public:
    explicit NoOverrun(size_t n) : q_(n) {}
    PUSH_WAIT
    POP(space_)

private:
    MEMBERS(RingNoOverrun<int>)
};

class OffByOne {
public:
    explicit OffByOne(size_t n) : q_(n) {}
    PUSH_WAIT
    POP(space_)

private:
    MEMBERS(RingOffByOne<int>)
};

class Closing {
public:
    explicit Closing(size_t n) : q_(n) {}
    PUSH_WAIT
    bool pop(int &out) {
        {
            std::unique_lock<std::mutex> l(m_);
            data_.wait(l, [this] { return !q_.empty() || stop_; });
            if (q_.empty()) return false;
            out = std::move(q_.front());
            q_.pop_front();
        }
        space_.notify_one();
        return true;
    }
    void close() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        data_.notify_all();
        space_.notify_all();
    }

private:
    MEMBERS(Ring<int>)
    bool stop_ = false;
};

class Disabled {
public:
    Disabled() : q_() {}
    PUSH_WAIT
    POP(space_)

private:
    MEMBERS(Ring<int>)
};

class Silent {
public:
    explicit Silent(size_t n) : q_(n) {}
    PUSH_WAIT
    POP(data_)

private:
    MEMBERS(Ring<int>)
};

int main() {
    int x = 0;
    Bounded a(4);
    a.push(1);
    a.offer(2);
    a.pop(x);
    a.pop_for(x, std::chrono::milliseconds(5));
    (void)a.size();
    (void)a.overruns();
    (void)a.discards();
    CountDecides b(4);
    b.push(1);
    b.pop(x);
    if (b.overruns() > 0) return 1;
    NoOverrun c(4);
    c.push(1);
    c.pop(x);
    OffByOne d(4);
    d.push(1);
    d.pop(x);
    Closing e(4);
    e.push(1);
    e.pop(x);
    e.close();
    Disabled f;
    f.push(1);
    f.pop(x);
    Silent g(4);
    g.push(1);
    g.pop(x);
}
