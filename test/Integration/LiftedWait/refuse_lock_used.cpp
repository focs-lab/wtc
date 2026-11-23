// Refused: the worker unlocks the wait's lock itself after the wait; the
// lifted wait never holds the mutex, and the unlock would release a
// mutex not held.
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (i) Pool::worker(int)::'lambda'()::operator() uses the wait's lock otherwise than to build, destroy and wait with it
// OUT: tasks 30000 sum 450015000

#include "Inputs/pool.h"
#include <cstdlib>

std::atomic<long> sum{0}, count{0};

class Pool {
public:
    explicit Pool(int n) { start(n); }
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

private:
    void start(int n) {
        for (int i = 0; i < n; ++i) worker(i);
    }
    void worker(int) {
        threads.emplace_back([this]() {
            int v;
            bool got = false;
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this, &v, &got]() { got = q.pop(v); return got || done; });
                    lock.unlock();
                }
                if (!got) return;
                sum += v;
                ++count;
            }
        });
    }
    std::vector<std::thread> threads;
    Queue<int> q;
    std::atomic<bool> done{false};
    std::mutex mutex;
    std::condition_variable cv;
};

int main(int argc, char **argv) {
    {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        for (int k = 1; k <= 30000; ++k) pool.push(k);
    }
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
