// Refused: as refuse_member_pointer.cpp, through a virtual call; the
// overrider is a function of a class derived from the call's, which the
// call reaches.
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 2 20 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (k) the section of mutex in Pool::finish calls Queue<int>::push (through a pointer to StopThenPut::run, Queue<int>::push), which may change what the predicate reads
// OUT: tasks 2020 expected 2020

#include "Inputs/pool.h"
#include <cstdlib>

std::atomic<long> count{0};
class Pool;
Pool *gpool;
struct Act { virtual void run(int) = 0; };
struct StopThenPut : Act { void run(int v) override; };

class Pool {
public:
    explicit Pool(int n) { start(n); }
    ~Pool() { for (auto &t : threads) t.join(); }
    void push(int v) {
        q.push(v);
        std::unique_lock<std::mutex> lock(mutex);
        cv.notify_one();
    }
    void finish(int v, Act *a) {
        std::unique_lock<std::mutex> lock(mutex);
        a->run(v);
        cv.notify_all();
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
void StopThenPut::run(int v) {
    gpool->done.store(true);
    for (volatile int i = 0; i < 2000; i = i + 1) {}
    gpool->q.push(v);
}
int main(int argc, char **argv) {
    int rounds = argc > 2 ? std::atoi(argv[2]) : 200;
    StopThenPut s;
    for (int r = 0; r < rounds; ++r) {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        gpool = &pool;
        for (int k = 1; k <= 100; ++k) pool.push(k);
        pool.finish(101, &s);
    }
    std::printf("tasks %ld expected %ld\n", count.load(), 101L * rounds);
}
