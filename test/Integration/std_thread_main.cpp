// main starts a std::thread and then keeps counting in a field next to the
// worker's. The worker cannot be resolved (see false_sharing.cpp), so the
// pair can only be possible -- but it must be reported.
//
// It was not. The start-up phase ends at the first spawn the module can
// see, and std::thread's spawn was invisible: the constructor's call to
// _M_start_thread was not recognised, so everything main did after it was
// taken for start-up, before any thread exists, and silenced. The call is
// now recognised by name.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s
//
// CHECK: possible false sharing in @stats
// CHECK-SAME: 'hits'
// CHECK-SAME: 'misses'
// CHECK: false sharing: 0 definite, 1 possible

#include <thread>

struct Stats { long hits; long misses; };
Stats stats;

void worker() {
    for (int i = 0; i < 1000; ++i) stats.misses = stats.misses + 1;
}

int main() {
    std::thread t(worker);
    for (int i = 0; i < 1000; ++i) stats.hits = stats.hits + 1;
    t.join();
}
