// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// Two producer threads, two consumer threads, one queue -> MPMC.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @producer_a() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @producer_b() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer_a() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @consumer_b() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %a = cir.get_global @producer_a : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%a) : (!cir.ptr<!cir.func<()>>) -> ()
    %b = cir.get_global @producer_b : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%b) : (!cir.ptr<!cir.func<()>>) -> ()
    %c = cir.get_global @consumer_a : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c) : (!cir.ptr<!cir.func<()>>) -> ()
    %d = cir.get_global @consumer_b : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%d) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// CHECK: Queue @q
// CHECK-NEXT: producers: 2
// CHECK-NEXT: consumers: 2
// CHECK-NEXT: classification: MPMC
