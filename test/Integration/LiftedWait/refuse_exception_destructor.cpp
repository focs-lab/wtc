// Refused: a section of the mutex drops the last reference to an exception
// pointer, which destroys the exception object and runs the program's
// destructor, which sets the stop flag and pushes the last task. Dropping
// an exception pointer may run the destructor of anything the program
// throws (std::make_exception_ptr names it), and the pass follows it.
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe 2 20 | FileCheck %s --check-prefix=OUT
//
// CHECK: locked wait cv in Pool not lifted: (k) the section of mutex in Pool::finish calls Queue<int>::push (through std::__exception_ptr::exception_ptr::
// OUT: tasks 2020 expected 2020

#include "Inputs/pool.h"
#include <cstdlib>
#include <exception>

std::atomic<long> count{0};
class Pool;
Pool *gpool;

struct Stopper {
    int v;
    bool armed;
    Stopper(int x) : v(x), armed(false) {}
    Stopper(const Stopper &o) : v(o.v), armed(true) {}
    ~Stopper();
};

class Pool {
public:
    explicit Pool(int n) { start(n); }
    ~Pool() { for (auto &t : threads) t.join(); }
    void push(int v) {
        q.push(v);
        std::unique_lock<std::mutex> lock(mutex);
        cv.notify_one();
    }
    void finish(std::exception_ptr &e) {
        std::unique_lock<std::mutex> lock(mutex);
        e = nullptr;  // the last reference: ~Stopper runs here
        cv.notify_all();
    }
    Queue<int> q;
    std::atomic<bool> done{false};
private:
    void start(int n) { for (int i = 0; i < n; ++i) worker(i); }
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
                ++count;
            }
        });
    }
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
};

Stopper::~Stopper() {
    if (!armed) return;
    gpool->done.store(true);
    for (volatile int i = 0; i < 2000; i = i + 1) {}
    gpool->q.push(v);
}

int main(int argc, char **argv) {
    int rounds = argc > 2 ? std::atoi(argv[2]) : 200;
    for (int r = 0; r < rounds; ++r) {
        Pool pool(argc > 1 ? std::atoi(argv[1]) : 2);
        gpool = &pool;
        std::exception_ptr e = std::make_exception_ptr(Stopper(101));
        for (int k = 1; k <= 100; ++k) pool.push(k);
        pool.finish(e);
    }
    std::printf("tasks %ld expected %ld\n", count.load(), 101L * rounds);
}
