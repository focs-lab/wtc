// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// Two independent queues in the same module, classified independently:
// q1 is SPSC (one producer, one consumer), q2 is MPMC (two producers, two
// consumers). Verifies root resolution correctly partitions accesses
// per-queue rather than pooling them together.

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

  cir.func @q2_consumer_a() {
    %queue = cir.get_global @q2 : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @q2_consumer_b() {
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
    %c2a = cir.get_global @q2_consumer_a : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c2a) : (!cir.ptr<!cir.func<()>>) -> ()
    %c2b = cir.get_global @q2_consumer_b : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c2b) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// Verdicts are printed in the order queues are first accessed, so each one
// is checked against its own queue: swapped verdicts fail.
// CHECK-LABEL: Queue @q1
// CHECK-NEXT: producers: 1
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: SPSC
// CHECK-LABEL: Queue @q2
// CHECK-NEXT: producers: 2
// CHECK-NEXT: consumers: 2
// CHECK-NEXT: classification: MPMC
