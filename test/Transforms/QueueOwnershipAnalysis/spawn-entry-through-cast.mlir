// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// The entry function reaches the spawn through a cast, which is what the
// two-argument overload produces. Requiring the operand to be a bare
// get_global loses the thread silently, and every access inside it then
// reports as having no thread context: a confusing symptom a long way from
// its cause.

!s32i = !cir.int<s, 32>
!void_fn = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.void>) [#cir.annotation<"wtc_thread_spawn">] {
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
    %p = cir.get_global @producer : !void_fn
    %pc = cir.cast bitcast %p : !void_fn -> !cir.ptr<!cir.void>
    cir.call @spawn(%pc) : (!cir.ptr<!cir.void>) -> ()
    %c = cir.get_global @consumer : !void_fn
    %cc = cir.cast bitcast %c : !void_fn -> !cir.ptr<!cir.void>
    cir.call @spawn(%cc) : (!cir.ptr<!cir.void>) -> ()
    cir.return
  }
}

// CHECK: queue @q: spsc (producers: 1, consumers: 1)
