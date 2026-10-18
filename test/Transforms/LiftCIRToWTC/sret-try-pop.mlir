// RUN: wtc-opt %s --lift-cir-to-wtc | FileCheck %s --check-prefix=LIFT
// RUN: wtc-opt %s --lift-cir-to-wtc --lower-wtc-to-cir | FileCheck %s --check-prefix=ROUND

// Reduced from real x86-64 output: there, try_pop returns its shared_ptr
// through a hidden first argument that the callee marks llvm.sret, so the
// call has no result and the queue is the second argument. Lifting used to
// call getResult() on that call and crash. It now lifts to a queue_pop that
// writes through the slot, and lowering puts the slot back first, sret
// included, since a call without it no longer matches its callee's ABI.
// (On AArch64 the same function returns its value directly; the
// round-trip test covers that shape.)

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
!shared_ptr_i32 = !cir.struct<"std::shared_ptr<int>" {data !cir.ptr<!s32i>, data !cir.ptr<!s32i>}>

module {
  cir.func private @try_pop(!cir.ptr<!shared_ptr_i32> {llvm.sret = !shared_ptr_i32}, !cir.ptr<!queue_i32>) [#cir.annotation<"wtc_queue_try_pop">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>) {
    %slot = cir.alloca "tmp" align(8) : !cir.ptr<!shared_ptr_i32>
    cir.call @try_pop(%slot, %q) : (!cir.ptr<!shared_ptr_i32> {llvm.sret = !shared_ptr_i32}, !cir.ptr<!queue_i32>) -> ()
    cir.return
  }
}

// LIFT-LABEL: cir.func @caller
// LIFT-SAME: (%[[Q:.*]]: !cir.ptr
// LIFT: %[[SLOT:.*]] = cir.alloca
// LIFT: %[[QUEUE:.*]] = builtin.unrealized_conversion_cast %[[Q]] {{.*}} to !wtc.queue<!s32i>
// LIFT: "wtc.queue_pop"(%[[QUEUE]], %[[SLOT]]) <{fallback_impl = @try_pop, {{.*}}}> : (!wtc.queue<!s32i>, !cir.ptr<{{.*}}>) -> ()

// ROUND-LABEL: cir.func @caller
// ROUND-SAME: (%[[Q:.*]]: !cir.ptr
// ROUND: %[[SLOT:.*]] = cir.alloca
// ROUND-NOT: wtc.queue_pop
// ROUND: cir.call @try_pop(%[[SLOT]], %[[Q]]) : (!cir.ptr<{{.*}}> {llvm.sret = {{.*}}}, !cir.ptr<{{.*}}>) -> ()
