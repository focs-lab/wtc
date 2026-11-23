// Refused: as refuse_aliased_flag.cpp, the waiting section changing the
// flag by one read-modify-write (exchange): the flag the predicate reads
// through the reference it captures is that member, and the waiting
// section may not change what the predicate reads.
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 4 2000 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Sig not lifted: (k) the section of Sig::start(int)::'lambda'()::operator() that waits changes Sig's member the predicate reads, and runs without mutex once lifted
// OUT: signals 2000

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

std::atomic<long> consumed{0};

class Sig {
public:
    explicit Sig(int n) {
        for (int i = 0; i < n; ++i) start(i);
    }
    ~Sig() {
        done = true;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.notify_all();
        }
        for (auto &t : threads) t.join();
    }
    void raise() {
        sig = true;
        std::unique_lock<std::mutex> lock(mutex);
        cv.notify_all();
    }
    bool pending() { return sig.load(); }

private:
    void start(int) {
        threads.emplace_back([this]() {
            std::atomic<bool> &s = sig;
            bool unused = false;  // a third capture: the closure is passed in memory
            while (true) {
                bool got;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this, &s, &unused]() { unused = false; return s.load() || done.load(); });
                    sig.exchange(false);
                    got = true;
                }
                if (got) ++consumed;
                if (done.load()) return;
            }
        });
    }
    std::vector<std::thread> threads;
    std::atomic<bool> sig{false};
    std::atomic<bool> done{false};
    std::mutex mutex;
    std::condition_variable cv;
};

int main(int argc, char **argv) {
    int n = argc > 1 ? std::atoi(argv[1]) : 4;
    long k = argc > 2 ? std::atol(argv[2]) : 20000;
    {
        Sig s(n);
        for (long i = 0; i < k; ++i) {
            s.raise();
            while (s.pending()) std::this_thread::yield();
        }
    }
    std::printf("signals %ld\n", k);
}
