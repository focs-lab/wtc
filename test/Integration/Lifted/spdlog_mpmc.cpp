// A reduced model of spdlog 1.14.1's asynchronous queue, read as written:
// mpmc_blocking_queue (details/mpmc_blocking_q.h, the non-MinGW branch)
// over circular_q (details/circular_q.h), inside the thread pool
// (details/thread_pool-inl.h) whose post chooses enqueue, enqueue_nowait or
// enqueue_if_have_room by the overflow policy and whose workers dequeue
// until a terminate message. The messages are reduced to an integer and a
// kind, the logger to the overflow policy it posts with; nothing is
// written for the compiler.
//
// The class lifts. circular_q is read from its member functions as a ring
// of capacity max_items (its constructor makes max_items + 1 slots, and
// full() is "the tail is one behind the head"); enqueue becomes a push
// that waits while the queue is full, enqueue_if_have_room a push that may
// decline, whose declined branch, the discard count, stays the program's
// own; enqueue_nowait a push that overruns the oldest; dequeue the blocking
// pop; the counters are handed back by the pool and nothing else, so the
// approximate overrun count is enough. The substitute's capacity is exact
// (runtime/bounded_ring.hpp).
//
// The overflow policy decides whether an overrun can happen. It is the
// logger's member, set when the logger is built (async_logger.h). In the
// first build the logger is built with the default policy, block, which
// the analysis resolves through the member, its constructor and the
// pool's post, so the overrunning push never runs and is left out: the
// loggers are producers only. In the second the policy is chosen at run time, the overrun may
// run, and every logger is a consumer as well; run with an argument it
// overruns a queue of sixteen, and loses messages, never more than it
// posted. The third has the worker of spdlog before 1.10, which waits with
// dequeue_for and a time limit of ten seconds.
//
// The workers are started by the pool's constructor in a loop over its
// count, and pop the queue object's queue through methods of the pool and
// of the queue object, so the queue is single-consumer when the pool has
// one worker, which the pool knows when it builds the queue object: the
// queue object's constructor, which builds the lifted queue, is called
// with the tag chosen there (BLOCK, TIMED). Where the loggers may overrun,
// they are consumers too and nothing is chosen (RUNTIME).
//
// The code kept is the upstream code, reduced; spdlog is under the MIT licence.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefixes=CHECK,BLOCK
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=CHECK,BLOCK %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
//
// RUN: %wtc_cc -DWTC_CHECK -DPOLICY_AT_RUN_TIME %s -o %t.rt.exe 2>&1 | FileCheck %s --check-prefixes=CHECK,RUNTIME
// RUN: %t.rt.exe | FileCheck %s --check-prefix=OUT
// RUN: %t.rt.exe overrun | FileCheck %s --check-prefix=DROPS
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g -DPOLICY_AT_RUN_TIME %s -o %t.rt.tsan.exe 2>&1 | FileCheck %s --check-prefixes=CHECK,RUNTIME %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.rt.tsan.exe overrun 2>&1 | FileCheck %s --check-prefixes=TSAN,DROPS %}
//
// RUN: %wtc_cc -DWTC_CHECK -DTIMED_WORKER %s -o %t.timed.exe 2>&1 | FileCheck %s --check-prefixes=CHECK,TIMED
// RUN: %t.timed.exe | FileCheck %s --check-prefix=OUT
//
// CHECK-NOT: locked queue {{.*}} not lifted
// BLOCK: enqueue_nowait never runs, so its overrun is left out (the call in thread_pool::post_async_msg_ never runs: the condition around it is always false)
// TIMED: enqueue_nowait never runs, so its overrun is left out
// RUNTIME: enqueue_nowait may overrun, so its callers are consumers too (the call in thread_pool::post_async_msg_ can run
// CHECK: locked queue q_ in spdlog::details::mpmc_blocking_queue<async_msg> lifted (
// BLOCK-SAME: dequeue: blocking pop
// RUNTIME-SAME: dequeue: blocking pop
// TIMED-SAME: dequeue_for: blocking pop with a time limit
// CHECK-SAME: enqueue: push that waits while full
// CHECK-SAME: enqueue_nowait: push that overruns the oldest when full
// CHECK-SAME: enqueue_if_have_room: push that discards when full
// CHECK: queue q_ in any spdlog::details::mpmc_blocking_queue<async_msg>: mpmc (producers: many, consumers: many)
// BLOCK: specialised at run time: mpsc when the loop at {{.*}} (consumers) runs at most once, else generic; chosen at the call at {{.*}}, which builds it
// TIMED: specialised at run time: mpsc when the loop at {{.*}} (consumers) runs at most once, else generic; chosen at the call at {{.*}}, which builds it
// RUNTIME-NOT: specialised at run time
// TSAN-NOT: ThreadSanitizer
// OUT: logged 30000 of 30000
// DROPS: logged {{[0-9]+}} of 30000{{(: none more)?$}}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace spdlog {
namespace details {

template <typename T>
class circular_q {
    size_t max_items_ = 0;
    typename std::vector<T>::size_type head_ = 0;
    typename std::vector<T>::size_type tail_ = 0;
    size_t overrun_counter_ = 0;
    std::vector<T> v_;

public:
    using value_type = T;

    // empty ctor - create a disabled queue with no elements allocated at all
    circular_q() = default;

    explicit circular_q(size_t max_items)
        : max_items_(max_items + 1)  // one item is reserved as marker for full q
          ,
          v_(max_items_) {}

    // push back, overrun (oldest) item if no room left
    void push_back(T &&item) {
        if (max_items_ > 0) {
            v_[tail_] = std::move(item);
            tail_ = (tail_ + 1) % max_items_;

            if (tail_ == head_)  // overrun last item if full
            {
                head_ = (head_ + 1) % max_items_;
                ++overrun_counter_;
            }
        }
    }

    const T &front() const { return v_[head_]; }
    T &front() { return v_[head_]; }

    size_t size() const {
        if (tail_ >= head_) {
            return tail_ - head_;
        } else {
            return max_items_ - (head_ - tail_);
        }
    }

    void pop_front() { head_ = (head_ + 1) % max_items_; }

    bool empty() const { return tail_ == head_; }

    bool full() const {
        // head is ahead of the tail by 1
        if (max_items_ > 0) {
            return ((tail_ + 1) % max_items_) == head_;
        }
        return false;
    }

    size_t overrun_counter() const { return overrun_counter_; }

    void reset_overrun_counter() { overrun_counter_ = 0; }
};

template <typename T>
class mpmc_blocking_queue {
public:
    using item_type = T;
    explicit mpmc_blocking_queue(size_t max_items)
        : q_(max_items) {}

    // try to enqueue and block if no room left
    void enqueue(T &&item) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            pop_cv_.wait(lock, [this] { return !this->q_.full(); });
            q_.push_back(std::move(item));
        }
        push_cv_.notify_one();
    }

    // enqueue immediately. overrun oldest message in the queue if no room left.
    void enqueue_nowait(T &&item) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            q_.push_back(std::move(item));
        }
        push_cv_.notify_one();
    }

    void enqueue_if_have_room(T &&item) {
        bool pushed = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (!q_.full()) {
                q_.push_back(std::move(item));
                pushed = true;
            }
        }

        if (pushed) {
            push_cv_.notify_one();
        } else {
            ++discard_counter_;
        }
    }

    // dequeue with a timeout.
    // Return true, if succeeded dequeue item, false otherwise
    bool dequeue_for(T &popped_item, std::chrono::milliseconds wait_duration) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (!push_cv_.wait_for(lock, wait_duration, [this] { return !this->q_.empty(); })) {
                return false;
            }
            popped_item = std::move(q_.front());
            q_.pop_front();
        }
        pop_cv_.notify_one();
        return true;
    }

    // blocking dequeue without a timeout.
    void dequeue(T &popped_item) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            push_cv_.wait(lock, [this] { return !this->q_.empty(); });
            popped_item = std::move(q_.front());
            q_.pop_front();
        }
        pop_cv_.notify_one();
    }

    size_t overrun_counter() {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        return q_.overrun_counter();
    }

    size_t discard_counter() { return discard_counter_.load(std::memory_order_relaxed); }

    size_t size() {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        return q_.size();
    }

    void reset_overrun_counter() {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        q_.reset_overrun_counter();
    }

    void reset_discard_counter() { discard_counter_.store(0, std::memory_order_relaxed); }

private:
    std::mutex queue_mutex_;
    std::condition_variable push_cv_;
    std::condition_variable pop_cv_;
    spdlog::details::circular_q<T> q_;
    std::atomic<size_t> discard_counter_{0};
};

}  // namespace details
}  // namespace spdlog

enum class async_msg_type { log, terminate };
enum class async_overflow_policy { block, overrun_oldest, discard_new };

struct async_msg {
    async_msg_type msg_type = async_msg_type::log;
    int payload = 0;
};

class thread_pool {
public:
    thread_pool(size_t q_max_items, size_t threads_n) : q_(q_max_items) {
        for (size_t i = 0; i < threads_n; i++)
            threads_.emplace_back([this] { this->worker_loop_(); });
    }
    ~thread_pool() {
        for (size_t i = 0; i < threads_.size(); i++)
            post_async_msg_(async_msg{async_msg_type::terminate, 0}, async_overflow_policy::block);
        for (auto &t : threads_) t.join();
    }
    void post_log(int v, async_overflow_policy p) { post_async_msg_(async_msg{async_msg_type::log, v}, p); }
    size_t overrun_counter() { return q_.overrun_counter(); }
    void reset_overrun_counter() { q_.reset_overrun_counter(); }
    size_t discard_counter() { return q_.discard_counter(); }
    void reset_discard_counter() { q_.reset_discard_counter(); }
    size_t queue_size() { return q_.size(); }

private:
    void post_async_msg_(async_msg &&new_msg, async_overflow_policy overflow_policy) {
        if (overflow_policy == async_overflow_policy::block) {
            q_.enqueue(std::move(new_msg));
        } else if (overflow_policy == async_overflow_policy::overrun_oldest) {
            q_.enqueue_nowait(std::move(new_msg));
        } else {
            q_.enqueue_if_have_room(std::move(new_msg));
        }
    }
    void worker_loop_() {
        while (process_next_msg_()) {
        }
    }
    bool process_next_msg_() {
        async_msg incoming_async_msg;
#ifdef TIMED_WORKER
        // spdlog before 1.10
        bool dequeued = q_.dequeue_for(incoming_async_msg, std::chrono::seconds(10));
        if (!dequeued) {
            return true;
        }
#else
        q_.dequeue(incoming_async_msg);
#endif
        if (incoming_async_msg.msg_type == async_msg_type::terminate) return false;
        logged.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    spdlog::details::mpmc_blocking_queue<async_msg> q_;
    std::vector<std::thread> threads_;

public:
    static std::atomic<long> logged;
};
std::atomic<long> thread_pool::logged{0};

// The logger keeps its overflow policy, given when it is built, and posts
// every message with it (async_logger.h, async_logger-inl.h).
class async_logger {
public:
    async_logger(thread_pool &tp, async_overflow_policy overflow_policy = async_overflow_policy::block)
        : tp_(tp), overflow_policy_(overflow_policy) {}
    void log(int v) { tp_.post_log(v, overflow_policy_); }

private:
    thread_pool &tp_;
    async_overflow_policy overflow_policy_;
};

int main(int argc, char **argv) {
    (void)argv;
#ifdef POLICY_AT_RUN_TIME
    const size_t capacity = 16;
#else
    (void)argc;
    const size_t capacity = 1024;
#endif
    {
        thread_pool pool(capacity, 1);
#ifdef POLICY_AT_RUN_TIME
        async_logger logger(pool, argc > 1 ? async_overflow_policy::overrun_oldest : async_overflow_policy::block);
#else
        async_logger logger(pool);
#endif
        std::vector<std::thread> loggers;
        for (int t = 0; t < 3; ++t)
            loggers.emplace_back([&] {
                for (int i = 0; i < 10000; ++i) logger.log(i);
            });
        for (auto &t : loggers) t.join();
    }
    const long logged = thread_pool::logged.load();
    if (logged == 30000)
        std::printf("logged %ld of 30000\n", logged);
    else
        std::printf("logged %ld of 30000: %s\n", logged, logged < 30000 ? "none more" : "more than posted");
}
