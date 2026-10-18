// RUN: wtc-opt %s --lower-wtc-to-cir | FileCheck %s

// wtc.queue_push has no result, but the function it lowers to may have one:
// an implementation substituted for the original can report whether the push
// succeeded. The lowered call has to match the callee's signature. It used to
// be built with no result whatever the callee returned.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>

module {
  cir.func private @push_returning_bool(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> !cir.bool [#cir.annotation<"wtc_queue_push">]
  cir.func private @push_returning_void(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>, %v: !cir.ptr<!s32i>) {
    %qcast = builtin.unrealized_conversion_cast %q : !cir.ptr<!queue_i32> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %v) {fallback_impl = @push_returning_bool, source_queue_type = !cir.ptr<!queue_i32>} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    "wtc.queue_push"(%qcast, %v) {fallback_impl = @push_returning_void, source_queue_type = !cir.ptr<!queue_i32>} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK-LABEL: cir.func @caller
// CHECK: cir.call @push_returning_bool({{.*}}) : (!cir.ptr<{{.*}}>, !cir.ptr<!s32i>) -> !cir.bool
// CHECK: cir.call @push_returning_void({{.*}}) : (!cir.ptr<{{.*}}>, !cir.ptr<!s32i>) -> ()
