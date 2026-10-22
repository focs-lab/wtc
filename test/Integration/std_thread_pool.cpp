// A thread pool shaped like spdlog's asynchronous logger: the queue is a
// member of the pool, the workers are started with std::thread in a
// counted loop over a number read at run time, each running a lambda that
// captured `this`, the pool is held by shared_ptr and the loggers reach it
// through weak_ptr. Nothing is written for the compiler: no shim spawn, no
// annotation. The queue is `q_ in any thread_pool`, two logging threads
// push to it, the workers pop from it, and the implementation is chosen
// when the pool is built: the single-consumer queue when the loop runs
// once, the general-purpose one otherwise. Every element arrives once.
// WTC_CHECK aborts if a second thread ever pops from the single-consumer
// queue.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 1 | FileCheck %s --check-prefix=ONE
// RUN: %t.exe 3 | FileCheck %s --check-prefix=THREE
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 1 2>&1 | FileCheck %s --check-prefixes=TSAN,ONE %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 3 2>&1 | FileCheck %s --check-prefixes=TSAN,THREE %}
//
// CHECK: queue q_ in any thread_pool: mpmc (producers: many, consumers: many)
// CHECK: queue q_ in any thread_pool: consumers: one thread per iteration of the loop at std_thread_pool.cpp:{{[0-9]+}}:{{[0-9]+}}, so one when it runs at most once
// CHECK: specialised at run time: mpsc when the loop at std_thread_pool.cpp:{{[0-9]+}}:{{[0-9]+}} (consumers) runs at most once, else generic
// TSAN-NOT: ThreadSanitizer
// ONE: tag 3 ok 200000
// THREE: tag 0 ok 200000

#include "queue.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

constexpr int kPerLogger = 100000;
std::atomic<long> sum{0}, count{0};

class thread_pool {
public:
    explicit thread_pool(size_t threads_n) {
        for (size_t i = 0; i < threads_n; i++)
            threads_.emplace_back([this] { this->worker_loop_(); });
    }
    ~thread_pool() {
        for (auto &t : threads_) t.join();
    }
    void post(int v) { q_.push(v); }
    int tag() const { return static_cast<int>(q_.tag()); }

private:
    void worker_loop_() {
        for (;;) {
            auto p = q_.try_pop();
            if (!p) { std::this_thread::yield(); continue; }
            if (*p == 0) return;   // one end marker per worker
            sum.fetch_add(*p, std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    wtc::queue<int> q_;
    std::vector<std::thread> threads_;
};

struct async_logger {
    std::weak_ptr<thread_pool> pool_;
    void log(int v) {
        if (auto p = pool_.lock()) p->post(v);
    }
};

int main(int argc, char **argv) {
    const size_t n = argc > 1 ? static_cast<size_t>(atoi(argv[1])) : 1;
    auto pool = std::make_shared<thread_pool>(n);
    async_logger first{pool}, second{pool};
    std::thread a([&] { for (int i = 1; i <= kPerLogger; ++i) first.log(i); });
    std::thread b([&] { for (int i = 1; i <= kPerLogger; ++i) second.log(i); });
    a.join();
    b.join();
    for (size_t i = 0; i < n; ++i) first.log(0);
    const int tag = pool->tag();
    pool.reset();   // joins the workers
    const long want = 2L * kPerLogger * (kPerLogger + 1) / 2;
    printf("tag %d ok %ld\n", tag, sum.load() == want ? count.load() : -1L);
}
