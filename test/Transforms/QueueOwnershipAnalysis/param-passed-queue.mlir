// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// The queue is stack-allocated in main (not a global) and handed to both
// worker functions as their sole parameter via spawn's context argument.
// Resolving producer/consumer's queue operand requires walking back through
// the spawn call's second operand to main's alloca -- a local trace alone
// (as used for the global-backed tests) cannot reach it, since it dead-ends
// at the block argument inside each worker.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, %arg: !cir.ptr<!s32i>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @producer(%queue: !cir.ptr<!s32i>) {
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer(%queue: !cir.ptr<!s32i>) {
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %q = cir.alloca !s32i, !cir.ptr<!s32i>, ["q"] {alignment = 4 : i64}
    %p = cir.get_global @producer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%p, %q) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%c, %q) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK: Queue
// CHECK-NEXT: producers: 1
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: SPSC
