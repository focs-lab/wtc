// Two counters in one struct, one thread each: the textbook shape, and the
// one every detector in the literature is demonstrated on.
//
// Nothing here is written for the compiler: no shim, no annotation, no
// pragma. The struct is what anyone would write, and the fact that the two
// fields are contended is invisible in the source. That is the point.
//
// The verdict here is `possible`, not `definite`, and that is the honest
// answer rather than a weaker one. std::thread's constructor hands the
// callable to the bodiless _M_start_thread inside a heap-allocated state
// object, so the analysis sees *that* a thread starts -- the call is
// recognised by name -- but not *which* function it runs. It knows only that
// the two functions have their addresses taken, which is equally true of two
// callbacks handed to one single-threaded event loop. The pthread_create
// companion test, where the entry is visible, says `definite`.
//
// std_thread_main.cpp checks the other half: that recognising the start is
// enough to end main's start-up phase.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s
//
// CHECK: possible false sharing in @{{.*}}stats
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
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
