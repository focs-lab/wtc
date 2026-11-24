// A waiter at real-time priority: the wait lifts as in pool.cpp, but its
// policy parks after its pause and never spins on, since a thread of
// real-time priority that yields keeps a producer of lower priority off its
// processor (docs/progress.md). The policy's input that says so is the
// same as a paced producer's. The thread raises its own priority, so the
// requirement holds from its start routine, the std::thread's _M_run.
// Every task still runs once; without the privilege to raise the priority
// the request fails and the pool runs as
// before.
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool lifted (wait in Pool::worker(int)::'lambda'()::operator(), 1 notify_one, 1 notify_all; 1 producers, waiters read when it is built; it parks after its pause, never spinning on, since a waiter is reached from std::thread::_State_impl<{{.*}}>::_M_run through {{.*}}Pool::worker(int)::'lambda'()::operator(), a thread at real-time priority (SCHED_FIFO, pthread_setschedparam))
// OUT: tasks 30000 sum 450015000

#include "Inputs/pool.h"
#include <cstdlib>
#include <pthread.h>
#include <sched.h>

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
            // Real-time priority, where the system allows it: what the
            // compiler reads is the request.
            sched_param sp{};
            sp.sched_priority = 1;
            pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
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
