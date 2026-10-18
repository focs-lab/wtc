// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// One thread calls the same helper twice, once per queue. Resolving the
// helper's parameter used to stop at the first entry matching the thread,
// so the second queue recorded no producer at all. Both queues are SPSC.

!s32i = !cir.int<s, 32>
!fn = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @q1 : !s32i
  cir.global "private" external @q2 : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !fn) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @push_to(%queue: !cir.ptr<!s32i>) {
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @producer() {
    %q1 = cir.get_global @q1 : !cir.ptr<!s32i>
    cir.call @push_to(%q1) : (!cir.ptr<!s32i>) -> ()
    %q2 = cir.get_global @q2 : !cir.ptr<!s32i>
    cir.call @push_to(%q2) : (!cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer1() {
    %queue = cir.get_global @q1 : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @consumer2() {
    %queue = cir.get_global @q2 : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %p = cir.get_global @producer : !fn
    cir.call @spawn(%p) : (!fn) -> ()
    %c1 = cir.get_global @consumer1 : !fn
    cir.call @spawn(%c1) : (!fn) -> ()
    %c2 = cir.get_global @consumer2 : !fn
    cir.call @spawn(%c2) : (!fn) -> ()
    cir.return
  }
}

// CHECK-LABEL: Queue @q1
// CHECK-NEXT: producers: 1
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: SPSC
// CHECK-LABEL: Queue @q2
// CHECK-NEXT: producers: 1
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: SPSC
