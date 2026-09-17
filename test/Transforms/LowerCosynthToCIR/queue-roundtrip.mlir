// RUN: cosynth-opt %s --lift-cir-to-cosynth --lower-cosynth-to-cir | FileCheck %s

// Lifting a push/try_pop call to cosynth.queue_push/queue_pop and then
// lowering back, with no intervening transform to pick a specialized
// implementation, should reconstruct calls to the exact original functions
// with the exact original operands -- a faithful round trip.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.record<struct "concur::queue<int>" {!cir.ptr<!s32i>}>
!shared_ptr_i32 = !cir.record<struct "std::shared_ptr<int>" {!cir.ptr<!s32i>, !cir.ptr<!s32i>}>

module {
  cir.func private @_ZN6concur5queueIiE4pushERKi(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) [#cir.annotation<"cosynth_queue_push">]
  cir.func private @_ZN6concur5queueIiE7try_popEv(!cir.ptr<!queue_i32>) -> !shared_ptr_i32 [#cir.annotation<"cosynth_queue_try_pop">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>, %v: !cir.ptr<!s32i>) {
    cir.call @_ZN6concur5queueIiE4pushERKi(%q, %v) : (!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> ()
    %popped = cir.call @_ZN6concur5queueIiE7try_popEv(%q) : (!cir.ptr<!queue_i32>) -> !shared_ptr_i32
    cir.return
  }
}

// CHECK-NOT: cosynth.queue_push
// CHECK-NOT: cosynth.queue_pop
// CHECK: cir.func @caller(%[[Q:.*]]: !cir.ptr{{.*}}, %[[V:.*]]: !cir.ptr{{.*}})
// CHECK: cir.call @_ZN6concur5queueIiE4pushERKi(%[[Q]], %[[V]])
// CHECK: cir.call @_ZN6concur5queueIiE7try_popEv(%[[Q]])
