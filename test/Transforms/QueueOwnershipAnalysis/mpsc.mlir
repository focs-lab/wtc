// RUN: cosynth-opt %s --queue-ownership-analysis | FileCheck %s

// Three producer threads, one consumer thread, one queue -> MPSC.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"cosynth_thread_spawn">] {
    cir.return
  }

  cir.func @producer_a() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !cosynth.queue<!s32i>
    "cosynth.queue_push"(%qcast, %value) : (!cosynth.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @producer_b() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !cosynth.queue<!s32i>
    "cosynth.queue_push"(%qcast, %value) : (!cosynth.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @producer_c() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !cosynth.queue<!s32i>
    "cosynth.queue_push"(%qcast, %value) : (!cosynth.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !cosynth.queue<!s32i>
    %popped = "cosynth.queue_pop"(%qcast) : (!cosynth.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %a = cir.get_global @producer_a : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%a) : (!cir.ptr<!cir.func<()>>) -> ()
    %b = cir.get_global @producer_b : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%b) : (!cir.ptr<!cir.func<()>>) -> ()
    %c = cir.get_global @producer_c : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c) : (!cir.ptr<!cir.func<()>>) -> ()
    %d = cir.get_global @consumer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%d) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// CHECK: Queue @q
// CHECK-NEXT: producers: 3
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: MPSC
