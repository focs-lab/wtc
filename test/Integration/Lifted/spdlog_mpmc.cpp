// A reduced model of spdlog 1.14.1's asynchronous queue, read as written:
// mpmc_blocking_queue (details/mpmc_blocking_q.h, the non-MinGW branch)
// over circular_q (details/circular_q.h), inside the thread pool
// (details/thread_pool-inl.h) whose post chooses enqueue, enqueue_nowait or
// enqueue_if_have_room by the overflow policy and whose workers dequeue
// until a terminate message. The messages are reduced to an integer and a
// kind.
//
// The class does not lift, and the remarks say why: circular_q is
// spdlog's own bounded ring, not a standard container; enqueue waits while
// it is full, enqueue_nowait overwrites the oldest message, and the
// counters of overruns and discards are read under the mutex. The bound and
// the overrun are the program's semantics, and the lock-free queues are
// unbounded, so substituting would change them: the pass refuses rather
// than approximate. The program runs as written.
//
// The code kept is the upstream code, reduced; spdlog is under the MIT licence.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK-DAG: locked queue q_ in spdlog::details::mpmc_blocking_queue<async_msg> not lifted: (e) the container spdlog::details::circular_q<async_msg> is not a standard container
// CHECK-DAG: locked queue q_ in spdlog::details::mpmc_blocking_queue<async_msg> not lifted: (e) spdlog::details::mpmc_blocking_queue<async_msg>::enqueue_if_have_room asks full(): the container is bounded
// CHECK-NOT: lifted (
// OUT: logged 30000

#include <atomic>
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
    circular_q() = default;
    explicit circular_q(size_t max_items) : max_items_(max_items + 1), v_(max_items_) {}

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
    const T &front() const { return v_[head_]; }
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

template <typename T>
class mpmc_blocking_queue {
public:
    using item_type = T;
    explicit mpmc_blocking_queue(size_t max_items) : q_(max_items) {}

    void enqueue(T &&item) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            pop_cv_.wait(lock, [this] { return !this->q_.full(); });
            q_.push_back(std::move(item));
        }
        push_cv_.notify_one();
    }

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
    size_t discard_counter() { return q_.discard_counter(); }

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
        q_.dequeue(incoming_async_msg);
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

int main() {
    {
        thread_pool pool(1024, 1);
        std::vector<std::thread> loggers;
        for (int t = 0; t < 3; ++t)
            loggers.emplace_back([&] {
                for (int i = 0; i < 10000; ++i) pool.post_log(i, async_overflow_policy::block);
            });
        for (auto &t : loggers) t.join();
    }
    std::printf("logged %ld\n", thread_pool::logged.load());
}
