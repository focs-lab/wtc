// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// A function whose control flow is branches rather than structured regions.
// Whether a spawn there runs once or many times cannot be read off the
// nesting, so the answer is many, which costs a specialisation and risks
// nothing. Note that this must key on the branching operations and not on
// the number of blocks: real code leaves an unreachable block after a
// return, and counting blocks would make every such function unstructured.

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
    cir.call @spawn(%p) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.br ^loop
  ^loop:
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// CHECK: queue @q: mpmc (producers: many, consumers: many)
