// The instructions that cannot wait, as an AArch64 fast path writes them:
// DPDK's rte_prefetch0 on arm64 is "PRFM PLDL1KEEP, [%0]", its barriers
// dmb and dsb with an option, and the spin hint yield. Waiting for an
// event, an exclusive load and a supervisor call are not cleared.
//
// RUN: %clangxx --target=aarch64-unknown-linux-gnu -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s

#include "progress.hpp"

long plain = 0;

// CHECK-DAG: remark: wtc: arm_hints(long*) is declared lock-free: nothing it reaches blocks
WTC_LOCK_FREE void arm_hints(long *p) {
    asm volatile("PRFM PLDL1KEEP, [%0]" : : "r"(p));
    asm volatile("prfm pstl1strm, %0" : : "Q"(*p));
    asm volatile("dmb ish" : : : "memory");
    asm volatile("dsb sy" : : : "memory");
    asm volatile("isb" : : : "memory");
    asm volatile("yield");
}

// CHECK-DAG: error: wtc: arm_wait_for_event() is declared lock-free but can block
WTC_LOCK_FREE void arm_wait_for_event() { asm volatile("wfe"); }

// CHECK-DAG: error: wtc: arm_exclusive(long*) is declared lock-free but can block
WTC_LOCK_FREE void arm_exclusive(long *p) {
    long v;
    asm volatile("ldaxr %0, [%1]" : "=r"(v) : "r"(p));
    plain = v;
}

// CHECK-DAG: error: wtc: arm_barrier_then_call() is declared lock-free but can block
WTC_LOCK_FREE void arm_barrier_then_call() { asm volatile("dmb ish; svc #0" : : : "memory"); }

// CHECK-DAG: error: wtc: arm_barrier_bad_option() is declared lock-free but can block
WTC_LOCK_FREE void arm_barrier_bad_option() { asm volatile("dmb ish, x0" : : : "memory"); }

int main() {
    arm_hints(&plain); arm_wait_for_event(); arm_exclusive(&plain); arm_barrier_then_call();
    arm_barrier_bad_option();
}
