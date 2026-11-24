// A spin lock taken by a signal handler and by the thread it interrupts:
// the handler waits for a holder it may have interrupted, and is reported.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: warning: wtc: on_signal(int) is a signal handler, installed by signal, so it must be async-signal-safe, but it can block
// CHECK: note: which spins until another thread changes what it reads (a compare-and-swap inside a helper, which the check cannot tell from a spin lock's test-and-set): it waits for another thread

#include <atomic>
#include <csignal>

struct spin {
    std::atomic<bool> f{false};
    bool try_lock() {
        bool e = false;
        return f.compare_exchange_strong(e, true, std::memory_order_acquire);
    }
    void lock() { while (!try_lock()) {} }
    void unlock() { f.store(false, std::memory_order_release); }
};

spin s;
long events;

void on_signal(int) {
    s.lock();
    ++events;
    s.unlock();
}

int main() {
    std::signal(SIGUSR1, on_signal);
    for (int i = 0; i < 1000000; ++i) {
        s.lock();
        ++events;
        s.unlock();
    }
}
