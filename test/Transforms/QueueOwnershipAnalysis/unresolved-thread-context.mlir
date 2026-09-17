// RUN: cosynth-opt %s --queue-ownership-analysis | FileCheck %s

// A queue_push with a resolvable queue root, but whose enclosing function
// was never spawned as a thread entry point, should be skipped with a
// diagnostic naming the orphaned caller.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"cosynth_thread_spawn">] {
    cir.return
  }

  cir.func @orphan() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !cosynth.queue<!s32i>
    "cosynth.queue_push"(%qcast, %value) : (!cosynth.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  // main() calls orphan() directly, not through the thread-spawn shim, so
  // no thread context is ever assigned to it.
  cir.func @main() {
    cir.call @orphan() : () -> ()
    cir.return
  }
}

// CHECK: Could not resolve thread context for orphan
// CHECK-NOT: classification:
