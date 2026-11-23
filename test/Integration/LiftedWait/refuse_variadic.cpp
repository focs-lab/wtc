// Refused: as refuse_member_pointer.cpp, through a pointer to a variadic
// function, which a call with more arguments than its parameters reaches.
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 2 20 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (k) the section of mutex in Pool::finish calls Queue<int>::push (through a pointer to stop_then_put, Queue<int>::push), which may change what the predicate reads
// OUT: tasks 2020 expected 2020

#include "Inputs/pool.h"
#include <cstdlib>

std::atomic<long> count{0};
class Pool;
Pool *gpool;
void stop_then_put(int n, ...);

class Pool {
public:
    explicit Pool(int n) { start(n); }
    ~Pool() { for (auto &t : threads) t.join(); }
    void push(int v) {
        q.push(v);
        std::unique_lock<std::mutex> lock(mutex);
        cv.notify_one();
    }
    void finish(int v, void (*a)(int, ...)) {
        std::unique_lock<std::mutex> lock(mutex);
        a(1, v);
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
#include <cstdarg>
void stop_then_put(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int v = va_arg(ap, int);
    va_end(ap);
    gpool->done.store(true);
    for (volatile int i = 0; i < 2000; i = i + 1) {}
    gpool->q.push(v);
}
int main(int argc, char **argv) {
    int rounds = argc > 2 ? std::atoi(argv[2]) : 200;
    for (int r = 0; r < rounds; ++r) {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        gpool = &pool;
        for (int k = 1; k <= 100; ++k) pool.push(k);
        pool.finish(101, &stop_then_put);
    }
    std::printf("tasks %ld expected %ld\n", count.load(), 101L * rounds);
}
