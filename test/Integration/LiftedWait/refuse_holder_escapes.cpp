// Refused: an object of a class that holds the pool, its address made a
// `void *` for a function with no body here: fwrite, which writes the
// holder's bytes (FWRITE), or mlock, which the pass has no model of
// (MLOCK), the holder nested 40 records deep (DEEP), or reached by
// dynamic_cast<void *> (DYNCAST); or a union that holds the pool, read
// through its other member (UNION). Lifted, the pool's condition variable
// holds a pointer to its waiter, and code the pass does not see, or the
// program itself, would read that where it reads the condition variable
// as written. Until 28 Nov 2026 (l) looked only at a pointer to the pool
// itself, and the waits of FWRITE, MLOCK and DEEP lifted (UNION's was
// refused, but only because the union refuses the queue class the
// predicate pops). A holder used only through the pool (the default),
// a pool held by a std::unique_ptr, whose deleter hands its address only
// to operator delete (UNIQUE, refused until 28 Nov 2026), and a
// std::optional of the pool (OPTIONAL), whose storage is a union of the
// pool and an empty member the front end builds with no step into it,
// lift.
//
// RUN: %wtc_cc --lift %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=LIFTS
// RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DFWRITE %s -o %t.fwrite.exe 2>&1 | FileCheck %s --check-prefix=FWRITE
// RUN: %t.fwrite.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DMLOCK %s -o %t.mlock.exe 2>&1 | FileCheck %s --check-prefix=MLOCK
// RUN: %t.mlock.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DDEEP %s -o %t.deep.exe 2>&1 | FileCheck %s --check-prefix=DEEP
// RUN: %t.deep.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DDYNCAST %s -o %t.dyncast.exe 2>&1 | FileCheck %s --check-prefix=DYNCAST
// RUN: %t.dyncast.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DUNION %s -o %t.union.exe 2>&1 | FileCheck %s --check-prefix=UNION
// RUN: %t.union.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DUNIQUE %s -o %t.unique.exe 2>&1 | FileCheck %s --check-prefix=LIFTS
// RUN: %t.unique.exe 4 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -DOPTIONAL %s -o %t.optional.exe 2>&1 | FileCheck %s --check-prefix=LIFTS
// RUN: %t.optional.exe 4 | FileCheck %s --check-prefix=OUT
//
// LIFTS: locked wait cv in Pool lifted
// FWRITE: locked wait cv in Pool not lifted: (l) main converts a pointer to Holder, which holds Pool, to another type
// MLOCK: locked wait cv in Pool not lifted: (l) main converts a pointer to Holder, which holds Pool, to another type
// DEEP: locked wait cv in Pool not lifted: (l) main converts a pointer to Deep<40>, which holds Pool, to another type
// DYNCAST: locked wait cv in Pool not lifted: (l) main converts a pointer to Holder, which holds Pool, to another type
// UNION: locked wait cv in Pool not lifted: (l) main reads a union that holds Pool through another of its members
// OUT: tasks 30000 sum 450015000

#include "Inputs/pool.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sys/mman.h>

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

#if defined(UNION)
union Holder {
    Pool pool;
    long words[4];
    explicit Holder(int n) : pool(n) {}
    ~Holder() { pool.~Pool(); }
};
#elif defined(DEEP)
template <int D>
struct Deep {
    Deep<D - 1> in;
    explicit Deep(int n) : in(n) {}
    Pool &get() { return in.get(); }
};
template <>
struct Deep<0> {
    Pool pool;
    explicit Deep(int n) : pool(n) {}
    Pool &get() { return pool; }
};
#else
struct Holder {
    int tag = 0;
    Pool pool;
    explicit Holder(int n) : pool(n) {}
#if defined(DYNCAST)
    virtual ~Holder() {}
#endif
};
#endif

int main(int argc, char **argv) {
    {
        const int n = argc > 1 ? std::atoi(argv[1]) : 2;
#if defined(DEEP)
        Deep<40> d(n);
        Pool &pool = d.get();
#elif defined(UNIQUE)
        std::unique_ptr<Pool> p(new Pool(n));
        Pool &pool = *p;
#elif defined(OPTIONAL)
        std::optional<Pool> o(std::in_place, n);
        Pool &pool = *o;
#else
        Holder h(n);
        Pool &pool = h.pool;
#endif
        for (int k = 1; k <= 30000; ++k) pool.push(k);
#if defined(FWRITE)
        if (argc > 5) std::fwrite(static_cast<void *>(&h), sizeof(Holder), 1, stderr);
#elif defined(MLOCK)
        if (argc > 5) mlock(static_cast<void *>(&h), sizeof(Holder));
#elif defined(DEEP)
        if (argc > 5) std::fwrite(static_cast<void *>(&d), sizeof(d), 1, stderr);
#elif defined(DYNCAST)
        if (argc > 5) std::fwrite(dynamic_cast<void *>(&h), sizeof(Holder), 1, stderr);
#elif defined(UNION)
        if (argc > 5) std::printf("%ld\n", h.words[0]);
#endif
    }
    std::printf("tasks %ld sum %ld\n", count.load(), sum.load());
}
