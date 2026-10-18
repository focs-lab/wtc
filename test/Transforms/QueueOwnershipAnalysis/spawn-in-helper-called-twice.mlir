// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// The same static spawn site runs twice without any loop: the helper that
// contains it is called from two places. Two producers, one queue, one
// consumer -- MPSC, however few spawn sites the IR has.

!s32i = !cir.int<s, 32>
!fn = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !fn) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @producer() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @start_producer() {
    %p = cir.get_global @producer : !fn
    cir.call @spawn(%p) : (!fn) -> ()
    cir.return
  }

  cir.func @main() {
    cir.call @start_producer() : () -> ()
    cir.call @start_producer() : () -> ()
    %c = cir.get_global @consumer : !fn
    cir.call @spawn(%c) : (!fn) -> ()
    cir.return
  }
}

// CHECK-LABEL: Queue @q
// CHECK-NEXT: producers: many
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: MPSC
