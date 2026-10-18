// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// A worker pool: one static spawn site inside a loop starts any number of
// producers. A thread id used to be handed out per static site, so the pool
// counted as one producer and the queue came out SPSC. Substituting a
// single-producer queue on that verdict introduces a race. The site now
// counts as possibly many threads.

!s32i = !cir.int<s, 32>
!fn = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

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

  cir.func @main(%more: !cir.bool) {
    cir.for : cond {
      cir.condition(%more)
    } body {
      %p = cir.get_global @producer : !fn
      cir.call @spawn(%p) : (!fn) -> ()
      cir.yield
    } step {
      cir.yield
    }
    %c = cir.get_global @consumer : !fn
    cir.call @spawn(%c) : (!fn) -> ()
    cir.return
  }
}

// CHECK-LABEL: THREAD T0
// CHECK-NEXT: entry: producer
// CHECK-NEXT: started: possibly more than once
// CHECK-LABEL: THREAD T1
// CHECK-NEXT: entry: consumer
// CHECK-NOT: started:
// CHECK-LABEL: Queue @q
// CHECK-NEXT: producers: many
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: MPSC
