// The wait lifts, and its policy's inputs: one producer that sleeps before
// every push, on every iteration of its loop, so paced (the policy spins,
// then parks, whatever the counts), and two workers started one by one,
// which the thread walk counts as four waiters (it over-counts here; the
// count only chooses a policy).
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 2 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool lifted (wait in Pool::worker(int)::'lambda'()::operator(), 1 notify_one, 1 notify_all; 1 producers, 4 waiters, a producer paced)
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
    void start(int) {
        worker(0);
        worker(1);
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
        for (int k = 1; k <= 30000; ++k) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(1));
            pool.push(k);
        }
    }
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
