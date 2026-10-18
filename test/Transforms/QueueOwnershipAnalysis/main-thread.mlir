// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// main is a thread too. Here main pushes (through a helper it calls
// directly), one worker pushes, and two pop: two producers against two
// consumers. main used to get no thread context, so its push was dropped
// and the verdict was SPMC.

!s32i = !cir.int<s, 32>
!fn = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !fn) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @push_one() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @worker() {
    cir.call @push_one() : () -> ()
    cir.return
  }

  cir.func @consumer() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %w = cir.get_global @worker : !fn
    cir.call @spawn(%w) : (!fn) -> ()
    %c = cir.get_global @consumer : !fn
    cir.call @spawn(%c) : (!fn) -> ()
    cir.call @spawn(%c) : (!fn) -> ()
    cir.call @push_one() : () -> ()
    cir.return
  }
}

// CHECK-LABEL: THREAD T3
// CHECK-NEXT: entry: main
// CHECK-NOT: Could not resolve
// CHECK-LABEL: Queue @q
// CHECK-NEXT: producers: 2
// CHECK-NEXT: consumers: 2
// CHECK-NEXT: classification: MPMC
