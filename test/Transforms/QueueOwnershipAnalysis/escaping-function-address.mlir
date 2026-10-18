// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// The producer is spawned once, but its address is also stored where the
// analysis cannot follow it, so it may run on threads that are never seen
// starting. The call graph is built from direct calls, which used to make
// that invisible and the queue SPSC. It is now UNKNOWN, and the function
// is named.

!s32i = !cir.int<s, 32>
!fn = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i
  cir.global "private" external @callback : !fn

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

  cir.func @main() {
    %p = cir.get_global @producer : !fn
    cir.call @spawn(%p) : (!fn) -> ()
    %slot = cir.get_global @callback : !cir.ptr<!fn>
    cir.store %p, %slot : !fn, !cir.ptr<!fn>
    %c = cir.get_global @consumer : !fn
    cir.call @spawn(%c) : (!fn) -> ()
    cir.return
  }
}

// CHECK: Could not resolve thread context for producer
// CHECK-LABEL: Queue @q
// CHECK: classification: UNKNOWN
