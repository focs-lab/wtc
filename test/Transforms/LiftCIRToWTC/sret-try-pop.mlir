// RUN: wtc-opt %s --lift-cir-to-wtc | FileCheck %s

// The shape the compiler actually emits for try_pop. Returning a
// shared_ptr by value means the object is written through a hidden first
// pointer argument, so the call has two operands and no result at all, and
// the queue is operand one. Reading operand zero here gets the return slot,
// and asking such a call for its result is how the earlier version crashed
// on the first real program it saw.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
!shared_ptr_i32 = !cir.struct<"std::shared_ptr<int>" {data !cir.ptr<!s32i>, data !cir.ptr<!s32i>}>

module {
  cir.func private @try_pop(!cir.ptr<!shared_ptr_i32>, !cir.ptr<!queue_i32>) [#cir.annotation<"wtc_queue_try_pop">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>, %slot: !cir.ptr<!shared_ptr_i32>) {
    cir.call @try_pop(%slot, %q) : (!cir.ptr<!shared_ptr_i32> {llvm.sret = !shared_ptr_i32}, !cir.ptr<!queue_i32>) -> ()
    cir.return
  }
}

// The lifted operation takes the queue from operand one and the return slot
// as its out operand, and has no result.
// CHECK: "wtc.queue_pop"(%[[Q:.*]], %[[SLOT:.*]]) <{fallback_impl = @try_pop
// CHECK-SAME: source_arg_attrs = [{llvm.sret = {{.*}}}, {}]
// CHECK-SAME: : (!wtc.queue<{{.*}}>, !cir.ptr<{{.*}}>) -> ()
