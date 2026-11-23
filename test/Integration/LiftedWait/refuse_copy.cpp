// Refused: the pool is made a `void *` and copied as bytes; a copy of the
// lifted pool's condition variable is a copy of a pointer to its waiter.
// The conversion is what the pass refuses: it reads the pool as another
// type.
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (l) main converts a pointer to Pool to another type
// OUT: tasks 30000 sum 450015000

#include "Inputs/pool.h"
#include <cstdlib>
#include <cstring>

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
        alignas(Pool) static unsigned char shadow[sizeof(Pool)];
        if (argc > 5) std::memcpy(shadow, static_cast<void *>(&pool), sizeof(Pool));
    }
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
