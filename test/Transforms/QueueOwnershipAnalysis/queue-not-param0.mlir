// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// The queue is the second parameter, not the first. Resolving a parameter by
// taking operand zero of the call regardless of which parameter it was
// attributes the access to whatever the first argument happens to be: here
// that is an integer, and the queue would record nothing.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q = #cir.int<0> : !s32i
  cir.global "private" external @v = #cir.int<0> : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, %arg: !cir.ptr<!s32i>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @push_tagged(%tag: !s32i, %queue: !cir.ptr<!s32i>) {
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @producer(%queue: !cir.ptr<!s32i>) {
    %tag = cir.const #cir.int<7> : !s32i
    cir.call @push_tagged(%tag, %queue) : (!s32i, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer(%queue: !cir.ptr<!s32i>) {
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %qg = cir.get_global @q : !cir.ptr<!s32i>
    %p = cir.get_global @producer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%p, %qg) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%c, %qg) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK: queue @q: spsc (producers: 1, consumers: 1)
