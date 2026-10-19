// A run of critical sections held as one, written by hand, goes through
// the whole pipeline -- lifted to the lock batch operations, lowered back
// to the shim's calls -- and two threads adding under it get the exact sum.
// The lifted operations are named in the IR, and none is left after
// lowering.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc -o - | FileCheck %s --check-prefix=LIFT
// RUN: %wtc_cc %s -o %t.exe && %t.exe | FileCheck %s
//
// LIFT: "wtc.lock_batch_begin"
// LIFT: "wtc.lock_batch_step"
// LIFT: "wtc.lock_batch_end"
// CHECK: total 10000100000 ok 1

#include "mutex.hpp"
#include "thread.hpp"

#include <cstdint>
#include <cstdio>

wtc::mutex m;
std::uint64_t total = 0;
constexpr std::uint64_t kCount = 100001;

void add() {
    wtc::mutex::batch b;
    m.batch_begin(b);
    for (std::uint64_t i = 0; i < kCount; ++i) {
        total += i;
        m.batch_step(b);
    }
    m.batch_end(b);
}

int main() {
    std::thread a = wtc::spawn(add);
    std::thread c = wtc::spawn(add);
    a.join();
    c.join();
    const std::uint64_t want = kCount * (kCount - 1);
    std::printf("total %llu ok %d\n", static_cast<unsigned long long>(total), total == want ? 1 : 0);
    return total == want ? 0 : 1;
}
