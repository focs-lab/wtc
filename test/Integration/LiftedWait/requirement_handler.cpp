// A wait a signal handler reaches is not lifted: the original wait was the
// handler's to answer for (the progress check warns about it), and the
// eventcount is not proven safe to reenter from a handler that interrupts
// its own thread inside the same wait, so the original is kept. Without
// the handler the same pool lifts.
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DNO_HANDLER %s -o %t.plain.exe 2>&1 | FileCheck %s --check-prefix=PLAIN
//
// CHECK: locked wait cv in Pool not lifted: the wait is reached from on_signal through Pool::take, a signal handler, installed by signal: the eventcount is not proven safe to reenter from a handler, so the original wait is kept
// PLAIN: locked wait cv in Pool lifted (wait in Pool::take,
// OUT: tasks 30000 sum 450015000

#include "Inputs/pool.h"
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
