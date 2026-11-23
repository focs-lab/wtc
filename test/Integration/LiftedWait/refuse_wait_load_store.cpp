// Refused: the waiting section counts its wake-ups with a load and a store
// of an atomic member. Under the mutex the two are one step, as two
// waiters' sections never interleave; lifted, the waiters run their
// sections at once and updates would be lost. Only one read-modify-write
// on each side of the wait is the original's one step.
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (k) the section of Pool::worker(int)::'lambda'()::operator() that waits calls std::__atomic_base<long>::load, and runs without mutex once lifted
// OUT: tasks 30000 wakeups counted 30000 (each task is one wake-up: expected 30000)

#include "Inputs/pool.h"
#include <cstdlib>

std::atomic<long> count{0};

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
    std::atomic<long> wakeups{0};  // guarded by mutex, atomic for n_idle-style readers

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
                    wakeups.store(wakeups.load() + 1);
                }
                if (!got) return;
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
    long w;
    {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 4);
        for (int k = 1; k <= 30000; ++k) pool.push(k);
        while (count.load() < 30000) std::this_thread::yield();
        w = pool.wakeups.load();
    }
    std::printf("tasks %ld wakeups counted %ld (each task is one wake-up: expected %ld)\n", count.load(), w, count.load());
}
