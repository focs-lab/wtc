// Refused: a pointer to an object that holds the pool, handed as itself
// to a function with no body here (DIRECT), or through a pointer that may
// reach one (INDIRECT, where sink's address is taken and the call fits
// it). Before 28 Nov 2026 the call through a pointer was not looked at,
// and its wait lifted. A
// program built with wtc-cc has such a function only with a file linked,
// which (g) refuses first, so the module is read as CIR here, the file
// that would define sink left out.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir -DDIRECT %s -o %t.direct.cir
// RUN: wtc-opt %t.direct.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=DIRECT
// RUN: %clangxx -std=c++17 -fclangir -emit-cir -DINDIRECT %s -o %t.indirect.cir
// RUN: wtc-opt %t.indirect.cir --wtc-lift-locked-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=INDIRECT
//
// DIRECT: locked wait cv in Pool not lifted: (l) main hands a pointer to Holder, which holds Pool, to sink, which has no body here
// INDIRECT: locked wait cv in Pool not lifted: (l) main hands a pointer to Holder, which holds Pool, to sink through a pointer, which has no body here

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

struct Holder {
    int tag = 0;
    Pool pool;
    explicit Holder(int n) : pool(n) {}
};

void sink(Holder *);  // defined in a file that is not linked

int main(int argc, char **argv) {
    {
        Holder h(argc > 1 ? std::atoi(argv[1]) : 2);
        for (int k = 1; k <= 30000; ++k) h.pool.push(k);
#if defined(DIRECT)
        sink(&h);
#else
        void (*volatile to)(Holder *) = sink;
        to(&h);
#endif
    }
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
