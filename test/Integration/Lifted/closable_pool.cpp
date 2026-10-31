// A pool over a closable queue of the common textbook shape: workers sleep
// in pop until a task or shutdown, close() sets the stop flag under the
// mutex and wakes every worker, and a worker returns once the queue is
// closed and empty. The class lifts to wtc::queue's blocking forms; every
// task runs exactly once, under ThreadSanitizer and with WTC_CHECK, with
// one worker and with several. One producer and many consumers: the
// queue whose consumers claim cells.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 1 | FileCheck %s --check-prefix=OUT
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 1 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 4 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
//
// CHECK: locked queue q_ in TaskQueue lifted (TaskQueue::push: push, TaskQueue::close: close, TaskQueue::pop: closable blocking pop)
// CHECK: queue q_ in any TaskQueue: spmc (producers: 1, consumers: many)
// CHECK: specialised: constructor tag rewritten to spmc
// TSAN-NOT: ThreadSanitizer
// OUT: tasks 20000 sum 200010000

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class TaskQueue {
public:
    void push(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> l(m_);
            q_.push_back(std::move(f));
        }
        cv_.notify_one();
    }
    bool pop(std::function<void()>& out) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !q_.empty() || stop_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void close() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
    }

private:
    std::deque<std::function<void()>> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

std::atomic<long> sum{0}, count{0};

int main(int argc, char** argv) {
    const size_t n = argc > 1 ? static_cast<size_t>(std::atoi(argv[1])) : 2;
    TaskQueue tasks;
    std::vector<std::thread> workers;
    for (size_t i = 0; i < n; ++i)
        workers.emplace_back([&] {
            std::function<void()> f;
            while (tasks.pop(f)) f();
        });
    for (long k = 1; k <= 20000; ++k) tasks.push([k] { sum += k; ++count; });
    tasks.close();
    for (auto& w : workers) w.join();
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
