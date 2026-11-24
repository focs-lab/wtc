/* A for loop bounded by an atomic, or a volatile, that another thread may
 * keep raising: no trip count, and its exit depends on what another thread
 * writes, a wait.
 *
 * RUN: %clang -std=c11 -I%shim -fclangir -emit-cir %s -o %t.cir
 * RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
 *
 * CHECK-DAG: error: wtc: drain is declared lock-free but can block
 * CHECK-DAG: error: wtc: drain_v is declared wait-free but can block
 * CHECK-DAG: note: which spins until another thread changes what it reads (an atomic or volatile load): it waits for another thread
 */

#include <stdatomic.h>
#define WTC_LOCK_FREE __attribute__((annotate("wtc_progress_lock_free")))
#define WTC_WAIT_FREE __attribute__((annotate("wtc_progress_wait_free")))

_Atomic int limit;
volatile int vlimit;
int out;

WTC_LOCK_FREE void drain(void) {
    for (int i = 0; i < limit; ++i) out += i;
}

WTC_WAIT_FREE void drain_v(void) {
    for (int i = 0; i < vlimit; ++i) out += i;
}

int main(void) { drain(); drain_v(); return 0; }
