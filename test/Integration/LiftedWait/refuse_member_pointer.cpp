// Refused: a section of the mutex sets the stop flag and pushes the last
// task through pointers to member functions. The front end passes the
// object of such a call as `void *`, which fits any function taking a
// pointer: the pass follows the call to every such function whose address
// the module takes, the push among them. Lifted, the predicate could see
// the flag set and the task not yet pushed, and the task would be lost.
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 2 20 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (k) the section of mutex in Pool::finish calls Queue<int>::
// OUT: tasks 2020 expected 2020

#include "Inputs/pool.h"
#include <cstdlib>

std::atomic<long> count{0};

class Pool;
Pool *gpool;
struct Ops {
    int unused;
    void put(int v);
    void stop();
};

class Pool {
public:
    explicit Pool(int n) { start(n); }
    ~Pool() {
        for (auto &t : threads) t.join();
    }
    void push(int v) {
        q.push(v);
        std::unique_lock<std::mutex> lock(mutex);
        cv.notify_one();
    }
    // The last task and the stop, in one section.
    void finish(int v, void (Ops::*put)(int), void (Ops::*halt)()) {
        Ops o{0};
        std::unique_lock<std::mutex> lock(mutex);
        (o.*halt)();
        for (volatile int i = 0; i < 2000; i = i + 1) {}
        (o.*put)(v);
        cv.notify_all();
    }
    Queue<int> q;
    std::atomic<bool> done{false};

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

void Ops::put(int v) { gpool->q.push(v); }
void Ops::stop() { gpool->done.store(true); }

int main(int argc, char **argv) {
    int rounds = argc > 2 ? std::atoi(argv[2]) : 200;
    for (int r = 0; r < rounds; ++r) {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        gpool = &pool;
        for (int k = 1; k <= 100; ++k) pool.push(k);
        pool.finish(101, &Ops::put, &Ops::stop);
    }
    std::printf("tasks %ld expected %ld\n", count.load(), 101L * rounds);
}
