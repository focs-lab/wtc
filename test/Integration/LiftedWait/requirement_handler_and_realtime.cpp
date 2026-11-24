// A wait reached from a signal handler and from a real-time thread whose
// policy is set first: every requirement that reaches it counts, and the
// handler's decides, so the wait is not lifted.
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
//
// CHECK: locked wait cv in Pool not lifted: the wait is reached from on_signal through Pool::take, a signal handler, installed by signal: the eventcount is not proven safe to reenter from a handler, so the original wait is kept

#include "Inputs/pool.h"
#include <pthread.h>
#include <sched.h>
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

// A real-time thread that takes from the same pool.
void *rt_taker(void *) {
    int v;
    the_pool->take(v);  // compile-only reproducer: not meant to be run
    return nullptr;
}

#ifndef NO_HANDLER
void on_signal(int) {
    int v;
    the_pool->take(v);
}
#endif

int main(int argc, char **argv) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    pthread_t rt;
    pthread_create(&rt, &a, rt_taker, nullptr);
#ifndef NO_HANDLER
    signal(SIGUSR1, on_signal);
#endif
    {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        the_pool = &pool;
        for (int k = 1; k <= 30000; ++k) pool.push(k);
        the_pool = nullptr;
    }
    pthread_join(rt, nullptr);
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
