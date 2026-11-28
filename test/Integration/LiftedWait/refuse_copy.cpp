// Refused: an object of a class that holds the pool is copied as bytes;
// a copy of the lifted pool's condition variable is a copy of a pointer to
// its waiter. The front end emits std::memcpy as a call of the C library's
// memcpy, not as cir.libc.memcpy, and until 28 Nov 2026 the pass looked
// only for the latter: the holder's copy was not seen, and its wait
// lifted. The pool itself copied (POOL) is refused for the conversion of
// its address to a void * as well. A pointer to a member of the pool
// (MEMBER) reaches the pass as a byte offset from the object's address,
// the front end having lowered it, and is refused as such a conversion.
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DPOOL %s -o %t.pool.exe 2>&1 | FileCheck %s --check-prefixes=POOL,CHECK
// RUN: %t.pool.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DMEMBER %s -o %t.member.exe 2>&1 | FileCheck %s --check-prefix=POOL
// RUN: %t.member.exe 4 | FileCheck %s --check-prefix=OUT
//
// POOL: locked wait cv in Pool not lifted: (l) main converts a pointer to Pool to another type
// CHECK: locked wait cv in Pool not lifted: (l) main copies or clears an object of Pool as bytes
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
    int tag = 0;

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

int main(int argc, char **argv) {
    {
        Holder h(argc > 1 ? std::atoi(argv[1]) : 2);
        Pool &pool = h.pool;
        for (int k = 1; k <= 30000; ++k) pool.push(k);
        alignas(Holder) static unsigned char shadow[sizeof(Holder)];
#if defined(POOL)
        if (argc > 5) std::memcpy(shadow, static_cast<void *>(&pool), sizeof(Pool));
#elif defined(MEMBER)
        int Pool::*tag = &Pool::tag;
        if (argc > 5) pool.*tag = 1;
#else
        if (argc > 5) std::memcpy(shadow, static_cast<void *>(&h), sizeof(Holder));
#endif
    }
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
