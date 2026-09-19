// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// Once a queue is classified, every push/pop op belonging to it should be
// stamped with a wtc.queue_kind attribute carrying that verdict, so a
// later selection/lowering pass can read it directly off each op instead
// of re-deriving the classification itself. Uses the same SPSC + MPSC
// shape as spsc.mlir/mpsc.mlir to also confirm two independently
// classified queues each get their own verdict, not a shared/conflated one.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q1 : !s32i
  cir.global "private" external @q2 : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @q1_producer() {
    %queue = cir.get_global @q1 : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @q1_consumer() {
    %queue = cir.get_global @q1 : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @q2_producer_a() {
    %queue = cir.get_global @q2 : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @q2_producer_b() {
    %queue = cir.get_global @q2 : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @q2_consumer() {
    %queue = cir.get_global @q2 : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %p1 = cir.get_global @q1_producer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%p1) : (!cir.ptr<!cir.func<()>>) -> ()
    %c1 = cir.get_global @q1_consumer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c1) : (!cir.ptr<!cir.func<()>>) -> ()
    %p2a = cir.get_global @q2_producer_a : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%p2a) : (!cir.ptr<!cir.func<()>>) -> ()
    %p2b = cir.get_global @q2_producer_b : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%p2b) : (!cir.ptr<!cir.func<()>>) -> ()
    %c2 = cir.get_global @q2_consumer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c2) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// CHECK: cir.func @q1_producer
// CHECK: wtc.queue_push"{{.*}}wtc.queue_kind = "SPSC"

// CHECK: cir.func @q1_consumer
// CHECK: wtc.queue_pop"{{.*}}wtc.queue_kind = "SPSC"

// CHECK: cir.func @q2_producer_a
// CHECK: wtc.queue_push"{{.*}}wtc.queue_kind = "MPSC"

// CHECK: cir.func @q2_producer_b
// CHECK: wtc.queue_push"{{.*}}wtc.queue_kind = "MPSC"

// CHECK: cir.func @q2_consumer
// CHECK: wtc.queue_pop"{{.*}}wtc.queue_kind = "MPSC"
