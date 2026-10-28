// Legal is not profitable: a loop the rules allow is batched only when the
// work between two of its critical sections, estimated over the CIR
// (Transforms/WorkEstimate.h), is under the threshold W8 measured
// (decisions/0006). Nothing here starts a thread, so the threads that take
// the mutex are not counted, and the threshold is the lowest of the host
// class's: 180 ns on the default class, sapphire-rapids, at 0.148 ns a
// unit. Every remark gives the estimate and the threshold, batched or not.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks -o %t.out.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir
//
// With the model off every legal loop is batched, as before the model, and
// the remark says nothing of the work (OFF). A threshold given outright
// replaces the table (GIVEN), and the constant of a faster host scales the
// estimate down (FAST).
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks=cost=off -o /dev/null 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks=threshold-ns=1000 -o /dev/null 2>&1 | FileCheck %s --check-prefix=GIVEN
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks=ns-per-unit=0.01 -o /dev/null 2>&1 | FileCheck %s --check-prefix=FAST
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks=cost=maybe -o /dev/null 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks=host=zen -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADHOST

#include "mutex.hpp"
#include "progress.hpp"

#include <cstring>

wtc::mutex m;
long total;
char dst[256], src[256];
extern "C" void opaque(int) noexcept;
static long mix(long x) { for (int k = 0; k < 200; ++k) x = x * 31 + k; return x; }
static long halve(long x) { return x > 1 ? halve(x / 2) : x; }

// No work: the loop's own condition and step, 6 units.
// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: critical sections in this loop batched: the mutex is let go every WTC_LOCK_BATCH_K sections and at the end; work between sections estimated at 0.9 ns (6 units), under the 180 ns threshold that holds for any number of threads
// OFF-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched: the mutex is let go every WTC_LOCK_BATCH_K sections and at the end{{$}}
WTC_BATCH void none(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; m.unlock(); } }

// Light work: eight rounds of arithmetic, 10 units each.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched: {{.*}}; work between sections estimated at 13 ns (89 units), under the 180 ns threshold that holds for any number of threads
WTC_BATCH void light(int n) { long x = 1; for (int i = 0; i < n; ++i) { m.lock(); total += x; m.unlock(); for (int k = 0; k < 8; ++k) x = x * 31 + k; } }

// Heavy work: a thousand rounds. Run under the mutex, they would hold up
// every other thread that wants it.
// CHECK-DAG: [[@LINE+4]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections estimated at 1481 ns (10009 units), over the 180 ns threshold that holds for any number of threads (the threads that take the mutex are not counted here)
// OFF-DAG: [[@LINE+3]]:{{.*}} remark: wtc: critical sections in this loop batched: the mutex is let go every WTC_LOCK_BATCH_K sections and at the end{{$}}
// GIVEN-DAG: [[@LINE+2]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections estimated at 1481 ns (10009 units), over the given threshold of 1000 ns
// FAST-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched: {{.*}}; work between sections estimated at 100 ns (10009 units), under the 180 ns threshold that holds for any number of threads
WTC_BATCH void heavy(int n) { long x = 1; for (int i = 0; i < n; ++i) { m.lock(); total += x; m.unlock(); for (int k = 0; k < 1000; ++k) x = x * 31 + k; } }

// A call with a body here costs that body: two hundred rounds.
// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections estimated at 298 ns (2016 units), over the 180 ns threshold
// GIVEN-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched: {{.*}}; work between sections estimated at 298 ns (2016 units), under the given threshold of 1000 ns
WTC_BATCH void calls(int n) { long x = 1; for (int i = 0; i < n; ++i) { m.lock(); total += x; m.unlock(); x = mix(x); } }

// An opaque call is refused before any estimate: the rules cannot clear it,
// with the model or without.
// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls opaque, which may wait for another thread
// OFF-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls opaque, which may wait for another thread
WTC_BATCH void opaque_call(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; m.unlock(); opaque(i); } }

// A call the rules clear can still have no bound: memcpy of a length known
// only at run time. With a constant length it is 2 units and one per 8
// bytes.
// CHECK-DAG: [[@LINE+3]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections not bounded (it calls memcpy with a length not known here), over the 180 ns threshold
// CHECK-DAG: [[@LINE+2]]:{{.*}} note: here
// OFF-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched
WTC_BATCH void copies(int n, unsigned len) { for (int i = 0; i < n; ++i) { m.lock(); ++total; m.unlock(); std::memcpy(dst, src, len); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched: {{.*}}; work between sections estimated at 2.5 ns (17 units)
WTC_BATCH void copies64(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; m.unlock(); std::memcpy(dst, src, 64); } }

// An inner loop whose trip count is known only at run time has no bound,
// however short it may turn out to be.
// CHECK-DAG: [[@LINE+3]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections not bounded (it runs a loop whose trip count is not known here), over the 180 ns threshold
// CHECK-DAG: [[@LINE+2]]:{{.*}} note: here
// OFF-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched
WTC_BATCH void unknown_trip(int n, int w) { long x = 1; for (int i = 0; i < n; ++i) { m.lock(); total += x; m.unlock(); for (int k = 0; k < w; ++k) x = x * 31 + k; } }

// Recursion has no bound either.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections not bounded (it calls halve(long), where it makes a recursive call to halve(long))
WTC_BATCH void recurses(int n) { for (int i = 0; i < n; ++i) { m.lock(); ++total; m.unlock(); total += halve(i); } }

// A branch counts its larger arm, however rarely it is taken: the estimate
// is of the worst iteration.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections estimated at 1482 ns (10013 units), over the 180 ns threshold
WTC_BATCH void rarely(int n) { long x = 1; for (int i = 0; i < n; ++i) { m.lock(); total += x; m.unlock(); if (i % 100 == 0) for (int k = 0; k < 1000; ++k) x = x * 31 + k; } }

// Work inside the critical section runs under the mutex either way, and is
// not counted.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched: {{.*}}; work between sections estimated at 0.9 ns (6 units)
WTC_BATCH void inside(int n) { long x = 1; for (int i = 0; i < n; ++i) { m.lock(); for (int k = 0; k < 1000; ++k) x = x * 31 + k; total += x; m.unlock(); } }

// A bound the counter reaches only by wrapping never ends the loop.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections not bounded (it runs a loop whose trip count is not known here)
WTC_BATCH void wraps_up(int n) { long x = 1; for (int i = 0; i < n; ++i) { m.lock(); total += x; m.unlock(); for (unsigned char k = 0; k <= 255; ++k) x += k; } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections not bounded (it runs a loop whose trip count is not known here)
WTC_BATCH void wraps_down(int n) { long x = 1; for (int i = 0; i < n; ++i) { m.lock(); total += x; m.unlock(); for (unsigned k = 4; k >= 0u; --k) x += k; } }

// IR-LABEL: cir.func {{.*}}@_Z4nonei(
// IR: "wtc.lock_batch_begin"
// IR-LABEL: cir.func {{.*}}@_Z5heavyi(
// IR-NOT: lock_batch
// IR: "wtc.lock"
// IR: "wtc.unlock"
// IR: cir.return

// BAD: error: wtc: --wtc-batch-locks: cost is estimate or off, not 'maybe'
// BADHOST: error: wtc: --wtc-batch-locks: no host class 'zen'; the classes are sapphire-rapids
