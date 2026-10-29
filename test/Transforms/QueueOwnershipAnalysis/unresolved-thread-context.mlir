// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// A queue_push with a resolvable queue root, but whose enclosing function
// was never spawned as a thread entry point, should be skipped with a
// diagnostic naming the orphaned caller.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q = #cir.int<0> : !s32i
  cir.global "private" external @v = #cir.int<0> : !s32i

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

  // main() calls orphan() directly, not through the thread-spawn shim, so
  // no thread context is ever assigned to it.
  cir.func @main() {
    cir.call @orphan() : () -> ()
    cir.return
  }
}

// main calls orphan directly, so orphan runs on the main thread, which is a
// thread like any other. An earlier version discarded this push with a
// message, which turned "main pushes too" into "nobody pushes".
// CHECK: queue @q: spsc (producers: 1, consumers: 0)
