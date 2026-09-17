// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// producer's queue arrives as a function parameter, but it's spawned via the
// zero-argument spawn() overload -- so its thread context resolves fine
// (proving this isn't the unresolved-thread-context case), yet nothing was
// ever bound to that parameter, so the queue root can't be traced past it.
// Should be skipped with a per-thread diagnostic, not crash or get silently
// attributed to some other queue.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @producer(%queue: !cir.ptr<!s32i>) {
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @main() {
    %p = cir.get_global @producer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%p) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>) -> ()
    cir.return
  }
}

// CHECK: THREAD T0
// CHECK: Could not resolve queue instance for T0
// CHECK-NOT: classification:
