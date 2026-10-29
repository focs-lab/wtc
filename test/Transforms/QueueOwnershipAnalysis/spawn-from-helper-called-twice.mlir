// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// The spawn sits once in a helper, and the helper is called twice. One
// static site, two threads. Multiplicity has to follow the chain of call
// sites and not just the enclosing loops, or a program can hide its second
// producer behind a function call.

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

  cir.func @start_producer() {
    %p = cir.get_global @producer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%p) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }

  cir.func @main() {
    cir.call @start_producer() : () -> ()
    cir.call @start_producer() : () -> ()
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// Two instances of the helper, so two distinct producer threads.
// CHECK: queue @q: mpsc (producers: many, consumers: 1)
