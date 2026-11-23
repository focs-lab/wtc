// Refused: a section's lock is let go of with std::unique_lock::release(),
// which keeps the mutex locked past the lock's scope, where the stop flag
// is set and the last task pushed, outside any section the pass reads. A
// lock object of the mutex may only be built and destroyed (and the
// wait's waited with).
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 2 20 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (i) Pool::hold uses a lock of mutex otherwise than to build and destroy it
// OUT: tasks 2020 expected 2020

#include "Inputs/pool.h"
#include <cstdlib>

std::atomic<long> count{0};
class Pool;
Pool *gpool;

class Pool {
public:
    explicit Pool(int n) { start(n); }
    ~Pool() { for (auto &t : threads) t.join(); }
    void push(int v) {
        q.push(v);
        std::unique_lock<std::mutex> lock(mutex);
        cv.notify_one();
    }
    std::mutex *hold() {
        std::unique_lock<std::mutex> lock(mutex);
        return lock.release();
    }
    void finish(int v) {
        std::mutex *held = hold();
        done.store(true);
        for (volatile int i = 0; i < 2000; i = i + 1) {}
        q.push(v);
        cv.notify_all();
        held->unlock();
    }
    Queue<int> q;
    std::atomic<bool> done{false};
private:
    void start(int n) { for (int i = 0; i < n; ++i) worker(i); }
    void worker(int) {
        threads.emplace_back([this]() {
            int v;
            bool got = false;
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this, &v, &got]() { got = q.pop(v); return got || done; });
                }
                if (!got) return;
                ++count;
            }
        });
    }
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
};
int main(int argc, char **argv) {
    int rounds = argc > 2 ? std::atoi(argv[2]) : 200;
    for (int r = 0; r < rounds; ++r) {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        gpool = &pool;
        for (int k = 1; k <= 100; ++k) pool.push(k);
        pool.finish(101);
    }
    std::printf("tasks %ld expected %ld\n", count.load(), 101L * rounds);
}
