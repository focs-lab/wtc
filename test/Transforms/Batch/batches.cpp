// Loops a WTC_BATCH function may have batched: a for loop, a while loop,
// and a loop that can break out, which still passes batch_end. The run is
// begun before the loop, every push becomes a deferred push, and the run
// ends after the loop.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-pushes -o %t.out.cir 2>&1 | FileCheck %s --check-prefix=REMARK
// RUN: FileCheck %s < %t.out.cir

#include "progress.hpp"
#include "queue.hpp"

wtc::queue<int> q;
int scale(int x) { return 3 * x + 1; }

// REMARK-COUNT-3: remark: wtc: pushes in this loop batched
WTC_BATCH void for_loop(int n) { for (int i = 0; i < n; ++i) q.push(scale(i)); }
WTC_BATCH void while_loop(int n) { int i = 0; while (i < n) { q.push(i); ++i; } }
WTC_BATCH void with_break(int n) { for (int i = 0;; ++i) { q.push(i); if (i == n) break; } }

// CHECK-LABEL: cir.func {{.*}}@_Z8for_loopi
// CHECK: cir.alloca "wtc.batch"
// CHECK: "wtc.queue_batch_begin"
// CHECK: cir.for
// CHECK: "wtc.queue_push_deferred"
// CHECK-NOT: "wtc.queue_push"(
// CHECK: "wtc.queue_batch_end"
// CHECK-LABEL: cir.func {{.*}}@_Z10while_loopi
// CHECK: "wtc.queue_batch_begin"
// CHECK: cir.while
// CHECK: "wtc.queue_batch_end"
// CHECK-LABEL: cir.func {{.*}}@_Z10with_breaki
// CHECK: "wtc.queue_batch_begin"
// CHECK: cir.break
// CHECK: "wtc.queue_batch_end"
