// main starts a std::thread and then keeps counting in a field next to the
// worker's. The pair is definite: the worker is the function the
// constructor was handed, resolved through the state object's vtable (see
// false_sharing.cpp).
//
// It was not reported at all once. The start-up phase ends at the first
// spawn the module can see, and std::thread's spawn was invisible: the
// constructor's call to _M_start_thread was not recognised, so everything
// main did after it was taken for start-up, before any thread exists, and
// silenced. The call is recognised by name, and the thread it starts is
// now resolved as well.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s
//
// CHECK: remark: wtc: false sharing in @stats
// CHECK-SAME: 'hits'
// CHECK-SAME: 'misses'
// CHECK: false sharing: 1 definite, 0 possible

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
