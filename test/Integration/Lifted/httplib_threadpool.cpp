// A reduced model of cpp-httplib's ThreadPool (httplib.h, 0.63.0), read as
// written: an accept loop enqueues one job per connection; base workers
// wait on cond_ for "a job or shutdown", dynamic workers, started by
// enqueue when jobs outnumber idle threads, wait the same way with a
// time-out and exit when it expires. Admission is bounded by
// max_queued_requests_. The model keeps enqueue, worker and shutdown and
// drops the virtual TaskQueue base and the OpenSSL clean-up.
//
// The class does not lift, and the remarks say why: the mutex also guards
// the idle-thread count and the lists of dynamic threads; enqueue decides
// on jobs_.size(), against the admission bound and against the idle count;
// and dynamic workers wait with a time-out. The shutdown flag alone would
// have been supported: it is set under the mutex with a broadcast, never
// cleared, and a worker returns only once the queue is empty. The program
// runs as written.
//
// The code kept is the upstream code, reduced; cpp-httplib is under the MIT licence.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK-DAG: locked queue jobs_ in ThreadPool not lifted: (c) the mutex mutex_ also guards idle_thread_count_ in ThreadPool::worker
// CHECK-DAG: locked queue jobs_ in ThreadPool not lifted: (d) ThreadPool::enqueue reads size(): the substitutes keep no linearisable count
// CHECK-DAG: locked queue jobs_ in ThreadPool not lifted: (d) ThreadPool::worker waits with a time-out (wait_for): the time-out path has no faithful equivalent yet
// CHECK-NOT: lifted (
// OUT: jobs 2000 refused 0

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <functional>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

class ThreadPool final {
public:
    explicit ThreadPool(size_t n, size_t max_n = 0, size_t mqr = 0, time_t idle_timeout_sec = 3)
        : base_thread_count_(n), max_queued_requests_(mqr), idle_timeout_sec_(idle_timeout_sec),
          idle_thread_count_(0), shutdown_(false) {
        max_thread_count_ = max_n == 0 ? n : max_n;
        threads_.reserve(base_thread_count_);
        for (size_t i = 0; i < base_thread_count_; i++) {
            threads_.emplace_back(std::thread([this]() { worker(false); }));
        }
    }
    ThreadPool(const ThreadPool &) = delete;
    ~ThreadPool() = default;

    bool enqueue(std::function<void()> fn) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (shutdown_) { return false; }
            if (max_queued_requests_ > 0 && jobs_.size() >= max_queued_requests_) {
                return false;
            }
            jobs_.push_back(std::move(fn));
            if (jobs_.size() > idle_thread_count_ &&
                threads_.size() + dynamic_threads_.size() < max_thread_count_) {
                cleanup_finished_threads();
                dynamic_threads_.emplace_back(std::thread([this]() { worker(true); }));
            }
        }
        cond_.notify_one();
        return true;
    }

    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cond_.notify_all();
        for (auto &t : threads_) {
            if (t.joinable()) { t.join(); }
        }
        std::list<std::thread> remaining_dynamic;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            remaining_dynamic = std::move(dynamic_threads_);
        }
        for (auto &t : remaining_dynamic) {
            if (t.joinable()) { t.join(); }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cleanup_finished_threads();
    }

private:
    void move_to_finished(std::thread::id id) {
        for (auto it = dynamic_threads_.begin(); it != dynamic_threads_.end(); ++it) {
            if (it->get_id() == id) {
                finished_threads_.push_back(std::move(*it));
                dynamic_threads_.erase(it);
                return;
            }
        }
    }

    void cleanup_finished_threads() {
        for (auto &t : finished_threads_) {
            if (t.joinable()) { t.join(); }
        }
        finished_threads_.clear();
    }

    void worker(bool is_dynamic) {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                idle_thread_count_++;
                if (is_dynamic) {
                    auto has_work = cond_.wait_for(lock, std::chrono::seconds(idle_timeout_sec_),
                                                   [&] { return !jobs_.empty() || shutdown_; });
                    if (!has_work) {
                        idle_thread_count_--;
                        move_to_finished(std::this_thread::get_id());
                        break;
                    }
                } else {
                    cond_.wait(lock, [&] { return !jobs_.empty() || shutdown_; });
                }
                idle_thread_count_--;
                if (shutdown_ && jobs_.empty()) { break; }
                fn = std::move(jobs_.front());
                jobs_.pop_front();
            }
            fn();
        }
    }

    size_t base_thread_count_;
    size_t max_thread_count_;
    size_t max_queued_requests_;
    time_t idle_timeout_sec_;
    size_t idle_thread_count_;
    bool shutdown_;
    std::list<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    std::list<std::thread> dynamic_threads_;
    std::vector<std::thread> finished_threads_;
    std::condition_variable cond_;
    std::mutex mutex_;
};

std::atomic<long> served{0};

int main() {
    ThreadPool pool(2, 4);
    int refused = 0;
    // The accept loop: one job per connection.
    for (int sock = 0; sock < 2000; ++sock)
        if (!pool.enqueue([] { ++served; })) ++refused;
    pool.shutdown();
    std::printf("jobs %ld refused %d\n", served.load(), refused);
}
