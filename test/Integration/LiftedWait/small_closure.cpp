// The wait lifts with a predicate whose closure is passed in registers: one
// that captures only `this` (8 bytes, one pointer) and one that captures
// two pointers (16 bytes). The front end passes such a closure as its
// bytes, one or two `void *` loaded from a copy of it; the pass reads the
// copy back to the closure the waiting function builds. No queue is
// popped, so the producers are not known and the policy parks after its
// pause.
//
// RUN: %wtc_cc --lift -O1 %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --lift --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
//
// CHECK-DAG: locked wait cv in Gate lifted (wait in Gate::wait, 0 notify_one, 1 notify_all; producers unknown
// CHECK-DAG: locked wait cv in Gate2 lifted (wait in Gate2::wait, 0 notify_one, 1 notify_all; producers unknown
// TSAN-NOT: ThreadSanitizer
// OUT: ok

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

struct Gate {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> open{false};
    void wait() {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this] { return open.load(); });
    }
    void release() {
        open = true;
        std::lock_guard<std::mutex> lk(m);
        cv.notify_all();
    }
};
struct Gate2 {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> open{false};
    void wait() {
        bool x = false;
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this, &x] { x = true; return open.load(); });
    }
    void release() {
        open = true;
        std::lock_guard<std::mutex> lk(m);
        cv.notify_all();
    }
};

int main() {
    Gate g;
    Gate2 g2;
    std::thread t([&] { g.wait(); g2.wait(); });
    g.release();
    g2.release();
    t.join();
    std::puts("ok");
}
