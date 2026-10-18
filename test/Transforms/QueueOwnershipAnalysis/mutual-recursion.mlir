// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// @a and @b call each other, and both push. The thread contexts used to be
// computed recursively with an empty placeholder cached while in progress:
// asking for @a first cached @b as reached by no thread at all, and @b's
// push was then discarded. Contexts are now computed to a fixed point, so
// the order the pushes are visited in no longer matters.

!s32i = !cir.int<s, 32>
!fn = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !fn) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @a() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.call @b() : () -> ()
    cir.return
  }

  cir.func @b() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.call @a() : () -> ()
    cir.return
  }

  cir.func @producer() {
    cir.call @a() : () -> ()
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
    %c = cir.get_global @consumer : !fn
    cir.call @spawn(%c) : (!fn) -> ()
    cir.return
  }
}

// CHECK-NOT: Could not resolve
// CHECK: QUEUE PUSH
// CHECK-NEXT: caller: a
// CHECK-NEXT: thread: T0
// CHECK: QUEUE PUSH
// CHECK-NEXT: caller: b
// CHECK-NEXT: thread: T0
// CHECK-LABEL: Queue @q
// CHECK-NEXT: producers: 1
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: SPSC
