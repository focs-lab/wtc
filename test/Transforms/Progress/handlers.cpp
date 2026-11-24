// A signal handler interrupts its own thread anywhere, so what it reaches
// must be async-signal-safe: the progress check holds every handler the
// program installs, by signal() or sigaction(), to lock-freedom without a
// first-use effect, and reports the first chain to what blocks. A handler
// that sets a flag is safe. One that allocates, one that takes a mutex to
// push a log line (as g3log's does), one that calls backtrace, and one
// that takes every worker's queue mutex (as h2o's SIGTERM handler does)
// are not. A handler that pushes to a queue is one more producer, and a
// queue with two consumers waits, in its pop, for the push the handler may
// have interrupted. The requirement is the program's own, unstated, so its
// violations are warnings, or errors when the build asks.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress=inferred-errors=true -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
//
// CHECK-DAG: remark: wtc: flag_handler(int) is a signal handler, installed by signal, so it must be async-signal-safe: it is
// CHECK-DAG: warning: wtc: allocating_handler(int) is a signal handler, installed by sigaction, so it must be async-signal-safe, but it can block
// CHECK-DAG: note: which calls the allocator (operator new[](unsigned long))
// CHECK-DAG: warning: wtc: logging_handler(int) is a signal handler, installed by sigaction, so it must be async-signal-safe, but it can block
// CHECK-DAG: note: which waits in the threading library (pthread_mutex_lock)
// CHECK-DAG: warning: wtc: tracing_handler(int) is a signal handler, installed by signal, so it must be async-signal-safe, but it can block
// CHECK-DAG: note: which calls the allocator: backtrace loads the unwinder and allocates (backtrace)
// CHECK-DAG: warning: wtc: notify_all_threads(int) is a signal handler, installed by signal, so it must be async-signal-safe, but it can block
// CHECK-DAG: remark: wtc: queue @events: mpmc (producers: many, consumers: many)
// CHECK-DAG: remark: wtc: queue @events: many producers: pushing_handler(int) may run any number of times: its address is taken, so it may run on any thread
// CHECK-DAG: warning: wtc: pushing_handler(int), a signal handler, installed by signal, pushes to a queue whose consumer waits for a push in progress (generic): a push the handler interrupts on its own thread holds a cell the consumer waits for until the handler returns; only the single-consumer queue (mpsc) and the rings of indices look past such a cell
// CHECK-DAG: note: where the handler is installed
//
// ERR-DAG: error: wtc: allocating_handler(int) is a signal handler, installed by sigaction, so it must be async-signal-safe, but it can block
// ERR-DAG: error: wtc: pushing_handler(int), a signal handler, installed by signal, pushes to a queue

#include "queue.hpp"
#include "thread.hpp"

#include <atomic>
#include <csignal>
#include <execinfo.h>
#include <mutex>
#include <pthread.h>

std::atomic<int> got{0};
std::mutex log_mutex;
long lines = 0;
struct receiver {
    pthread_mutex_t mutex;
    int pending;
} receivers[4];
wtc::queue<int> events;

void flag_handler(int) { got.store(1, std::memory_order_relaxed); }

void allocating_handler(int) {
    char *p = new char[64];
    got.store(p[0], std::memory_order_relaxed);
}

void logging_handler(int) {
    std::lock_guard<std::mutex> g(log_mutex);
    ++lines;
}

void tracing_handler(int) {
    void *frames[16];
    got.store(backtrace(frames, 16), std::memory_order_relaxed);
}

void notify_all_threads(int) {
    for (receiver &r : receivers) {
        pthread_mutex_lock(&r.mutex);
        ++r.pending;
        pthread_mutex_unlock(&r.mutex);
    }
}

void pushing_handler(int) { events.push(1); }

void consumer() { events.try_pop(); }

int main() {
    signal(SIGUSR1, flag_handler);
    struct sigaction sa {};
    sa.sa_handler = allocating_handler;
    sigaction(SIGUSR2, &sa, nullptr);
    struct sigaction sb {};
    sb.sa_handler = logging_handler;
    sigaction(SIGTERM, &sb, nullptr);
    signal(SIGHUP, tracing_handler);
    signal(SIGINT, notify_all_threads);
    signal(SIGALRM, pushing_handler);
    std::thread a = wtc::spawn(consumer), b = wtc::spawn(consumer);
    events.push(2);
    a.join();
    b.join();
}
