// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// The producer's address is stored in a global, which is what a table of
// callbacks or a virtual dispatch looks like once the names are gone. It may
// be called from anywhere, so it is many threads, and the queue is not
// single-producer however few calls this module happens to contain.

!s32i = !cir.int<s, 32>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i
  cir.global external @table = #cir.global_view<@producer> : !fnptr

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] {
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
    %c = cir.get_global @consumer : !fnptr
    cir.call @spawn(%c) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: address of this function is taken
// CHECK: queue @q: mpsc (producers: many, consumers: 1)
