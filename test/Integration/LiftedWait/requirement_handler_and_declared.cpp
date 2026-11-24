// A wait reached from a function declared lock-free and from a signal
// handler: the handler's requirement decides, whichever is met first, and
// the wait is not lifted.
//
// RUN: not %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
//
// CHECK: locked wait cv in Pool not lifted: the wait is reached from on_signal through Pool::take, a signal handler, installed by signal: the eventcount is not proven safe to reenter from a handler, so the original wait is kept

#include "Inputs/pool.h"
#include "progress.hpp"
#include <csignal>
#include <cstdlib>

std::atomic<long> sum{0}, count{0};

class Pool {
public:
    explicit Pool(int n) {
        for (int i = 0; i < n; ++i)
            threads.emplace_back([this]() {
                int v;
                while (take(v)) {
                    sum += v;
                    ++count;
                }
            });
    }
    ~Pool() {
        done = true;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.notify_all();
        }
        for (auto &t : threads) t.join();
    }
    void push(int v) {
        q.push(v);
        std::unique_lock<std::mutex> lock(mutex);
        cv.notify_one();
    }
    bool take(int &out) {
        int v = 0;
        bool got = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this, &v, &got]() { got = q.pop(v); return got || done; });
        }
        out = v;
        return got;
    }

private:
    std::vector<std::thread> threads;
    Queue<int> q;
    std::atomic<bool> done{false};
    std::mutex mutex;
    std::condition_variable cv;
};

Pool *the_pool = nullptr;

// A declared requirement that reaches the same wait, defined anywhere:
// annotated roots are listed before handlers.
WTC_LOCK_FREE void stats() {
    int v;
    the_pool->take(v);
}

#ifndef NO_HANDLER
void on_signal(int) {
    int v;
    the_pool->take(v);
}
#endif

int main(int argc, char **argv) {
#ifndef NO_HANDLER
    signal(SIGUSR1, on_signal);
#endif
    {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        the_pool = &pool;
        for (int k = 1; k <= 30000; ++k) pool.push(k);
        the_pool = nullptr;
    }
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
