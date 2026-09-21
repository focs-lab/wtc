// RUN: wtc-opt %s --lift-cir-to-wtc --lower-wtc-to-cir --reconcile-unrealized-casts | FileCheck %s

// A call with a hidden return slot, lifted and lowered again, must come back
// with the slot in the same place and the attribute that marks it intact.
// Lowering used to drop argument attributes, which would leave the caller
// and the callee disagreeing about the calling convention: a call that looks
// right and passes the object in the wrong place.

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

// CHECK-NOT: wtc.
// CHECK: cir.func @caller(%[[Q:.*]]: !cir.ptr<{{.*}}queue{{.*}}>, %[[SLOT:.*]]: !cir.ptr<{{.*}}shared_ptr{{.*}}>)
// CHECK: cir.call @try_pop(%[[SLOT]], %[[Q]])
// CHECK-SAME: llvm.sret
