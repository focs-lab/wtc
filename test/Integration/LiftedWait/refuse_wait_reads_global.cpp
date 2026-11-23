// Refused: the section that waits reads two globals another section of
// the mutex writes. Under the mutex a == b always; lifted, the waiter would
// read them without the mutex and could see the other section half done.
// The waiting section may read and write only its locals.
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (k) the section of Pool::worker(int)::'lambda'()::operator() that waits reads memory other than its locals, and runs without mutex once lifted
// OUT: tasks 30000 torn 0

#include "Inputs/pool.h"
#include <cstdlib>

long ga = 0, gb = 0;  // guarded by Pool::mutex
std::atomic<long> torn{0}, count{0};

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
    void tick() {
        std::lock_guard<std::mutex> lock(mutex);
        ga = ga + 1;
        for (volatile int i = 0; i < 50; i = i + 1) {}
        gb = gb + 1;
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
                long a, b;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this, &v, &got]() { got = q.pop(v); return got || done; });
                    a = ga;
                    b = gb;
                }
                if (a != b) ++torn;
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
    {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 4);
        std::thread ticker([&] { for (int k = 0; k < 20000; ++k) pool.tick(); });
        for (int k = 1; k <= 30000; ++k) pool.push(k);
        ticker.join();
    }
    std::printf("tasks %ld torn %ld\n", count.load(), torn.load());
}
