// A reduced model of CTPL (ctpl_stl.h, 437e135), read as written: the
// queue class detail::Queue has only a mutex, and the pool waits on a
// condition variable of its own, under a mutex of its own, with the
// queue's pop as the predicate. The pool's threads are started in a
// counted loop by resize(); push wraps each task in a heap-allocated
// std::function and pushes the pointer; stop(true) sets isDone, wakes
// every worker, joins them and clears what is left. The model keeps that
// structure and drops the futures. Nothing is written for the compiler.
//
// The queue class lifts without a blocking form: push and the try-pop
// match, and the element, a pointer, is copied out, which is moving it.
// The pool's own wait is left as it is: the lifted pop is non-blocking,
// as the original was, and a pusher still takes the pool's mutex before
// it notifies, so the worker's check and its sleep stay atomic with
// respect to the push. Every task runs once, under ThreadSanitizer and with
// WTC_CHECK too.
//
// The code kept is the upstream code, reduced; CTPL is under the Apache License 2.0.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 1 | FileCheck %s --check-prefix=OUT
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 4 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
//
// CHECK: locked queue q in ctpl::detail::Queue<std::function<void (int)> *> lifted (ctpl::detail::Queue<std::function<void (int)>*>::pop: try-pop, ctpl::detail::Queue<std::function<void (int)>*>::push: push)
// CHECK: queue q in any ctpl::detail::Queue<std::function<void (int)> *>: mpmc (producers: many, consumers: many)
// CHECK-NOT: not lifted
// TSAN-NOT: ThreadSanitizer
// OUT: tasks 20000 sum 200010000

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ctpl {
namespace detail {
template <typename T>
class Queue {
public:
    bool push(T const & value) {
        std::unique_lock<std::mutex> lock(this->mutex);
        this->q.push(value);
        return true;
    }
    // deletes the retrieved element, do not use for non integral types
    bool pop(T & v) {
        std::unique_lock<std::mutex> lock(this->mutex);
        if (this->q.empty())
            return false;
        v = this->q.front();
        this->q.pop();
        return true;
    }
    bool empty() {
        std::unique_lock<std::mutex> lock(this->mutex);
        return this->q.empty();
    }
private:
    std::queue<T> q;
    std::mutex mutex;
};
}  // namespace detail

class thread_pool {
public:
    explicit thread_pool(int nThreads) { this->nWaiting = 0; this->isStop = false; this->isDone = false; this->resize(nThreads); }
    ~thread_pool() { this->stop(true); }

    void resize(int nThreads) {
        int oldNThreads = static_cast<int>(this->threads.size());
        this->threads.resize(nThreads);
        this->flags.resize(nThreads);
        for (int i = oldNThreads; i < nThreads; ++i) {
            this->flags[i] = std::make_shared<std::atomic<bool>>(false);
            this->set_thread(i);
        }
    }

    void clear_queue() {
        std::function<void(int id)> * _f;
        while (this->q.pop(_f))
            delete _f;
    }

    void stop(bool isWait = false) {
        if (this->isDone || this->isStop)
            return;
        this->isDone = true;
        {
            std::unique_lock<std::mutex> lock(this->mutex);
            this->cv.notify_all();
        }
        for (int i = 0; i < static_cast<int>(this->threads.size()); ++i) {
            if (this->threads[i]->joinable())
                this->threads[i]->join();
        }
        this->clear_queue();
        this->threads.clear();
        this->flags.clear();
    }

    template <typename F>
    void push(F && f) {
        auto _f = new std::function<void(int id)>(std::forward<F>(f));
        this->q.push(_f);
        std::unique_lock<std::mutex> lock(this->mutex);
        this->cv.notify_one();
    }

private:
    void set_thread(int i) {
        std::shared_ptr<std::atomic<bool>> flag(this->flags[i]);
        auto f = [this, i, flag]() {
            std::atomic<bool> & _flag = *flag;
            std::function<void(int id)> * _f;
            bool isPop = this->q.pop(_f);
            while (true) {
                while (isPop) {
                    std::unique_ptr<std::function<void(int id)>> func(_f);
                    (*_f)(i);
                    if (_flag)
                        return;
                    else
                        isPop = this->q.pop(_f);
                }
                std::unique_lock<std::mutex> lock(this->mutex);
                ++this->nWaiting;
                this->cv.wait(lock, [this, &_f, &isPop, &_flag](){ isPop = this->q.pop(_f); return isPop || this->isDone || _flag; });
                --this->nWaiting;
                if (!isPop)
                    return;
            }
        };
        this->threads[i].reset(new std::thread(f));
    }

    std::vector<std::unique_ptr<std::thread>> threads;
    std::vector<std::shared_ptr<std::atomic<bool>>> flags;
    detail::Queue<std::function<void(int id)> *> q;
    std::atomic<bool> isDone;
    std::atomic<bool> isStop;
    std::atomic<int> nWaiting;
    std::mutex mutex;
    std::condition_variable cv;
};
}  // namespace ctpl

std::atomic<long> sum{0}, count{0};

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 2;
    constexpr int kTasks = 20000;
    {
        ctpl::thread_pool pool(n);
        std::thread other([&] {
            for (int k = 1; k <= kTasks / 2; ++k)
                pool.push([k](int) { sum += k; ++count; });
        });
        for (int k = kTasks / 2 + 1; k <= kTasks; ++k)
            pool.push([k](int) { sum += k; ++count; });
        other.join();
    }  // stop(true): every task queued before it runs
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
