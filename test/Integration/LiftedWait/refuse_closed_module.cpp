// Refused under (g), for each way the module may not be the whole program
// as far as the pool goes: compiled to be linked later (wtc-cc -c), with
// no main, or with a member function of the pool defined in another unit.
// Another unit's code may notify or wait on the pool's condition variable
// as written, which the lifted pool holds a pointer to its waiter in.
//
// RUN: %wtc_cc --lift -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=OPEN
// RUN: %clangxx -std=c++17 -fclangir -emit-cir -DNO_MAIN %s -o %t.nomain.cir
// RUN: wtc-opt %t.nomain.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOMAIN
// RUN: %clangxx -std=c++17 -fclangir -emit-cir -DNO_BODY %s -o %t.nobody.cir
// RUN: wtc-opt %t.nobody.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOBODY
//
// OPEN: locked wait cv in Pool not lifted: (g) the unit is compiled to be linked with files the pass does not see, which may use Pool as written
// NOMAIN: locked wait cv in Pool not lifted: (g) the module has no main: the rest of the program may use Pool as written
// NOBODY: locked wait cv in Pool not lifted: (g) Pool::stats has no body here: another unit defines it

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
#ifdef NO_BODY
    void stats();  // defined in another unit
#endif
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

#ifdef NO_MAIN
int run(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        for (int k = 1; k <= 30000; ++k) pool.push(k);
#ifdef NO_BODY
        pool.stats();
#endif
    }
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
    return 0;
}
