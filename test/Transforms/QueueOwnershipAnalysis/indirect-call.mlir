// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// A call through a function pointer. The call graph is built from symbol
// references, so an indirect call is not an edge, and a producer reached
// only that way would simply not be counted. It is counted instead by the
// other rule: a function whose address is taken for anything but a spawn
// may run on any thread, any number of times.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q = #cir.int<0> : !s32i
  cir.global "private" external @v = #cir.int<0> : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"wtc_thread_spawn">] {
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

  cir.func @main() {
    %p = cir.get_global @producer : !cir.ptr<!cir.func<()>>
    cir.call %p() : (!cir.ptr<!cir.func<()>>) -> ()
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// CHECK: address of this function is taken
// CHECK: queue @q: mpsc (producers: many, consumers: 1)
