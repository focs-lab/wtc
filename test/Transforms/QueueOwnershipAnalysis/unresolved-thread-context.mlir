// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// A push in a function nothing in this module calls or spawns. It may be
// entered from outside on any number of threads, so the push cannot be
// attributed to a thread: the function is named, and the queue it touches
// gets no verdict that could justify substituting it.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @orphan() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK: Could not resolve thread context for orphan
// CHECK-LABEL: Queue @q
// CHECK: classification: UNKNOWN
