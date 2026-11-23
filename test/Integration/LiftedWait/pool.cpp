// The wait lifts: the pool's mutex and condition variable serve only the
// workers' wait for "a task, or stop", whose predicate pops a queue that is
// atomic on its own and reads an atomic flag; the pushes notify under the
// mutex and change nothing else there; the stop flag is set before the
// notification, outside the mutex. One thread pushes, and the workers are
// started by a counted loop whose bound the constructor is given, so the
// policy reads that count when the pool is built. Every task runs once,
// as written, under ThreadSanitizer too.
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 1 | FileCheck %s --check-prefix=OUT
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %t.exe 9 | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --lift --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 4 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
//
// CHECK: locked queue q in Queue<int> lifted
// CHECK: locked wait cv in Pool lifted (wait in Pool::worker(int)::'lambda'()::operator(), 1 notify_one, 1 notify_all; 1 producers, waiters read when it is built)
// TSAN-NOT: ThreadSanitizer
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
