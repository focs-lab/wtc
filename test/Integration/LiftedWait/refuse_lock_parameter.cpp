// Refused: the wait's lock is built by the worker and handed to the
// function that waits by reference. The rewrite builds the wait's lock
// without the mutex in the waiting function; one built elsewhere, which
// other code may use, is not a lock it can place.
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (i) Pool::wait_with waits with a lock that is not a std::unique_lock it builds on mutex
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
                    wait_with(lock, v, got);
                }
                if (!got) return;
                sum += v;
                ++count;
            }
        });
    }
    void wait_with(std::unique_lock<std::mutex> &lock, int &v, bool &got) {
        cv.wait(lock, [this, &v, &got]() { got = q.pop(v); return got || done; });
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
