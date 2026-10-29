// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// A worker pool: one spawn site inside a loop stands for as many threads as
// the loop runs. Counting sites rather than threads called this
// single-producer, which is not a slower program but a racing one, and it is
// the commonest shape there is.

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
    cir.scope {
      cir.for : cond {
        %t = cir.const #cir.bool<true> : !cir.bool
        cir.condition(%t)
      } body {
        %p = cir.get_global @producer : !cir.ptr<!cir.func<()>>
        cir.call @spawn(%p) : (!cir.ptr<!cir.func<()>>) -> ()
        cir.yield
      } step {
        cir.yield
      }
    }
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// CHECK: queue @q: mpsc (producers: many, consumers: 1)
