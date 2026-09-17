// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// The same producer/consumer functions are each spawned twice, bound to two
// DIFFERENT stack-allocated queues at their respective spawn call sites. A
// single static queue_push (and queue_pop) op is reached by both thread
// instances of its function, but must resolve to two separate roots -- one
// per spawn's own bound argument. If resolution were cached per function
// instead of per (op, thread instance), both queues would incorrectly merge
// into one MPMC-looking queue instead of two independent SPSC queues.

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
    %q1 = cir.alloca !s32i, !cir.ptr<!s32i>, ["q1"] {alignment = 4 : i64}
    %q2 = cir.alloca !s32i, !cir.ptr<!s32i>, ["q2"] {alignment = 4 : i64}
    %p = cir.get_global @producer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%p, %q1) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.call @spawn(%p, %q2) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%c, %q1) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.call @spawn(%c, %q2) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// Two independent SPSC queues, not one shared MPMC queue.
// CHECK-COUNT-2: classification: SPSC
// CHECK-NOT: classification: MPMC
