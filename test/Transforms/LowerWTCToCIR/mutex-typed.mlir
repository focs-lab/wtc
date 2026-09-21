// RUN: wtc-opt %s --lift-cir-to-wtc --lower-wtc-to-cir --reconcile-unrealized-casts | FileCheck %s
// RUN: wtc-opt %s --lift-cir-to-wtc | FileCheck %s --check-prefix=LIFTED

// The mutex operations take a typed handle now. They used to accept any
// type at all, so an operation holding a raw pointer, or an integer, would
// verify happily; the queue operations were constrained and these were not,
// for no reason beyond oversight.

!s32i = !cir.int<s, 32>
!mutex = !cir.struct<"wtc::mutex" {data !s32i}>

module {
  cir.func private @lock(!cir.ptr<!mutex>) [#cir.annotation<"wtc_mutex_lock">]
  cir.func private @unlock(!cir.ptr<!mutex>) [#cir.annotation<"wtc_mutex_unlock">]

  cir.func @caller(%m: !cir.ptr<!mutex>) {
    cir.call @lock(%m) : (!cir.ptr<!mutex>) -> ()
    cir.call @unlock(%m) : (!cir.ptr<!mutex>) -> ()
    cir.return
  }
}

// LIFTED: "wtc.lock"(%{{.*}}) <{fallback_impl = @lock
// LIFTED-SAME: (!wtc.mutex) -> ()
// LIFTED: "wtc.unlock"(%{{.*}}) <{fallback_impl = @unlock

// CHECK-NOT: wtc.
// CHECK: cir.call @lock(%[[M:.*]])
// CHECK: cir.call @unlock(%[[M]])
