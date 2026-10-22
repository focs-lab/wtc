// Two counters in one struct, one thread each: the textbook shape, and the
// one every detector in the literature is demonstrated on.
//
// Nothing here is written for the compiler: no shim, no annotation, no
// pragma. The struct is what anyone would write, and the fact that the two
// fields are contended is invisible in the source. That is the point.
//
// The verdict is `definite`, as for the pthread_create companion test.
// std::thread's constructor hands the callable to the bodiless
// _M_start_thread inside a heap-allocated state object, and the new thread
// runs that state's _M_run, which nothing names but the state's vtable:
// the entry is read from there, and the function the thread runs from the
// constructor's operand. The verdict used to be `possible`, which was the
// honest answer then: the analysis saw that a thread started but not which
// function it ran, and knew only that both functions had their addresses
// taken, which is equally true of two callbacks handed to one
// single-threaded event loop.
//
// std_thread_main.cpp checks the other half: that recognising the start is
// enough to end main's start-up phase.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s
//
// CHECK: remark: wtc: false sharing in @{{.*}}stats
// CHECK-SAME: 'hits'
// CHECK-SAME: 'misses'

#include <thread>

struct Stats { long hits; long misses; };
static Stats stats;

void count_hits()   { for (int i = 0; i < 1000; ++i) ++stats.hits; }
void count_misses() { for (int i = 0; i < 1000; ++i) ++stats.misses; }

int main() {
    std::thread a(count_hits), b(count_misses);
    a.join();
    b.join();
    return static_cast<int>(stats.hits + stats.misses);
}
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
