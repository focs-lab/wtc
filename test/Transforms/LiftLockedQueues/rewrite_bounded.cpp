// What the pass writes for a bounded ring, with the runtime instantiated by
// hand the way wtc-cc instantiates it: the class's constructor builds the
// queue through wtc::lifted_bounded<C>::construct with the generic tag, the
// capacity the class passed to the ring, and whether a push may overrun;
// the waiting push and the pop become the runtime's; the push that may
// decline is replaced in place, its critical section by try_push, whose
// answer goes to the class's flag, and its notification by nothing, so
// that what the class does with a declined push (count it) stays. The
// overrunning push is called only where a value the analysis resolves says
// it does not run, so it is lifted as a plain push and the queue is built
// without overruns. The lifting pass raises the pushes to
// wtc.queue_push marked bounded, the waiting one marked wait and the one
// that may decline with its answer. The call of put that never runs still
// counts main as a producer (the analysis of threads does not read
// conditions), so with the producer thread there are two and one consumer:
// the queue is mpsc.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues -o %t.lifted.cir 2>&1 | FileCheck %s --check-prefix=REMARK
// RUN: FileCheck %s < %t.lifted.cir
// RUN: wtc-opt %t.lifted.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o %t.spec.cir 2>&1 | FileCheck %s --check-prefix=SPEC
// RUN: FileCheck %s --check-prefix=OPS < %t.spec.cir
//
// REMARK: Box::put never runs, so its overrun is left out (the call in main never runs: the condition around it is always false)
// REMARK: locked queue q_ in Box lifted (
//
// CHECK-LABEL: cir.func {{.*}}@_ZN3BoxC2Em(
// CHECK: %[[Q:.*]] = cir.get_member %{{.*}}[{{[0-9]+}}] {name = "q_"}
// CHECK: %[[CAP:.*]] = cir.load {{.*}} : !cir.ptr<!u64i>, !u64i
// CHECK: %[[TAG:.*]] = cir.const #cir.int<0>
// CHECK: %[[OVER:.*]] = cir.const #false
// CHECK: cir.call @{{.*}}14lifted_bounded{{.*}}9construct{{.*}}(%[[Q]], %[[TAG]], %[[CAP]], %[[OVER]])
// CHECK-LABEL: cir.func {{.*}}@_ZN3Box4pushEOi(
// CHECK-NEXT: %[[AT:.*]] = cir.get_member %arg0[{{[0-9]+}}] {name = "q_"}
// CHECK-NEXT: cir.call @{{.*}}14lifted_bounded{{.*}}9push_wait{{.*}}(%[[AT]], %arg1)
// CHECK-LABEL: cir.func {{.*}}@_ZN3Box5offerEOi(
// CHECK: %[[FLAG:.*]] = cir.alloca "pushed"
// CHECK: %[[OK:.*]] = cir.call @{{.*}}14lifted_bounded{{.*}}8try_push{{.*}}(
// CHECK: cir.store %[[OK]], %[[FLAG]]
// CHECK-NOT: unique_lock
// CHECK-NOT: notify_one
// CHECK: } else {
// CHECK: atomic_base{{.*}}pp
//
// SPEC: queue q_ in any Box: mpsc (producers: many, consumers: 1)
// SPEC: specialised: constructor tag rewritten to mpsc
//
// OPS: "wtc.queue_push"({{.*}}) <{bounded, {{.*}}wait}>
// OPS: "wtc.queue_push"({{.*}}) <{bounded, {{.*}}}> : {{.*}} -> !cir.bool

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include "lifted.hpp"

template <typename T>
class Ring {
    size_t max_items_ = 0;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t overrun_counter_ = 0;
    std::vector<T> v_;

public:
    using value_type = T;
    explicit Ring(size_t max_items) : max_items_(max_items + 1), v_(max_items_) {}
    void push_back(T &&item) {
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
    void pop_front() { head_ = (head_ + 1) % max_items_; }
    bool empty() const { return tail_ == head_; }
    bool full() const {
        if (max_items_ > 0) return ((tail_ + 1) % max_items_) == head_;
        return false;
    }
};

class Box {
public:
    explicit Box(size_t n) : q_(n) {}
    void push(int &&v) {
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
        else ++discards_;
    }
    void put(int &&v) {
        {
            std::unique_lock<std::mutex> l(m_);
            q_.push_back(std::move(v));
        }
        data_.notify_one();
    }
    void pop(int &out) {
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
    Ring<int> q_;
    std::atomic<size_t> discards_{0};
};

template struct wtc::lifted_bounded<Ring<int>>;

int main() {
    Box b(4);
    int mode = 0;  // a value the analysis resolves: put never runs
    if (mode == 2) b.put(3);
    std::thread consumer([&] {
        int x;
        for (int i = 0; i < 2; ++i) b.pop(x);
    });
    std::thread producer([&] {
        b.push(1);
        b.offer(2);
    });
    producer.join();
    consumer.join();
}
