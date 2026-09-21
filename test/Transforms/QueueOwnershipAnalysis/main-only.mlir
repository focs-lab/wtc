// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// One thread that both pushes and pops, and no other. That is a degenerate
// single-producer single-consumer queue and the ring is correct for it, so
// the verdict is spsc rather than a refusal. It is also indistinguishable
// from a genuine two-thread case by counting alone, which is fine here and
// worth knowing.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func @main() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }
}

// CHECK: queue @q: spsc (producers: 1, consumers: 1)
