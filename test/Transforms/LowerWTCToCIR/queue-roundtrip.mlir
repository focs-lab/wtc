// RUN: wtc-opt %s --lift-cir-to-wtc --lower-wtc-to-cir | FileCheck %s

// Lifting a push/try_pop call to wtc.queue_push/queue_pop and then
// lowering back, with no intervening transform to pick a specialized
// implementation, should reconstruct calls to the exact original functions
// with the exact original operands -- a faithful round trip.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
!shared_ptr_i32 = !cir.struct<"std::shared_ptr<int>" {data !cir.ptr<!s32i>, data !cir.ptr<!s32i>}>

module {
  cir.func private @_ZN3wtc5queueIiE4pushERKi(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push">]
  cir.func private @_ZN3wtc5queueIiE7try_popEv(!cir.ptr<!queue_i32>) -> !shared_ptr_i32 [#cir.annotation<"wtc_queue_try_pop">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>, %v: !cir.ptr<!s32i>) {
    cir.call @_ZN3wtc5queueIiE4pushERKi(%q, %v) : (!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> ()
    %popped = cir.call @_ZN3wtc5queueIiE7try_popEv(%q) : (!cir.ptr<!queue_i32>) -> !shared_ptr_i32
    cir.return
  }
}

// CHECK-NOT: wtc.queue_push
// CHECK-NOT: wtc.queue_pop
// CHECK: cir.func @caller(%[[Q:.*]]: !cir.ptr{{.*}}, %[[V:.*]]: !cir.ptr{{.*}})
// CHECK: cir.call @_ZN3wtc5queueIiE4pushERKi(%[[Q]], %[[V]])
// CHECK: cir.call @_ZN3wtc5queueIiE7try_popEv(%[[Q]])
