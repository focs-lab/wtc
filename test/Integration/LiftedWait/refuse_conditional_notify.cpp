// Refused: each waiter counts itself in nWaiting in its section, and a
// push notifies only when that count, read under the mutex, is positive.
// As written the mutex orders the two. Lifted, the waiter would count
// itself and look at the queue without the mutex, and a push that reads
// the count with no fence between could miss it and skip the
// notification, leaving the waiter asleep on a task (store buffering).
// Nothing else may touch what the waiting section changes.
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (k) the section of mutex in Pool::push touches nWaiting, which the waiting section changes, which it no longer excludes once lifted
// CHECK: locked wait cv in Pool not lifted: (h) Pool::push, which notifies cv, touches nWaiting, which the waiting section changes: a notification decided on it may miss a waiter once lifted
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
        if (nWaiting.load() > 0) cv.notify_one();
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
                    ++nWaiting;
                    cv.wait(lock, [this, &v, &got]() { got = q.pop(v); return got || done; });
                    --nWaiting;
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
    std::atomic<int> nWaiting{0};
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
