// Refused: another file is linked with this one; its code may use the
// pool's condition variable as written, which the lifted pool holds a
// pointer in.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/other.cpp -o %t.b.o
// RUN: %wtc_cc --lift %s %t.b.o -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (g) files are linked with this one, whose code may use the condition variable of Pool as written
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

extern "C" int other_value();
int main(int argc, char **argv) {
    if (other_value() != 7) return 1;
    {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        for (int k = 1; k <= 30000; ++k) pool.push(k);
    }
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
