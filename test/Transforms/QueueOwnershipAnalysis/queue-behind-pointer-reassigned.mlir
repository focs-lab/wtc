// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// The same slot written twice, so which queue the load yields depends on
// where in the function it is read. The forwarding rule requires a single
// store precisely so that this case stops rather than picks one, and the
// access becomes unresolvable instead of being attributed to a guess.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @v : !s32i
  cir.global "private" external @other : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, %arg: !cir.ptr<!s32i>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @producer(%queue: !cir.ptr<!s32i>) {
    %slot = cir.alloca "q.addr" align(8) : !cir.ptr<!cir.ptr<!s32i>>
    cir.store %queue, %slot : !cir.ptr<!s32i>, !cir.ptr<!cir.ptr<!s32i>>
    %o = cir.get_global @other : !cir.ptr<!s32i>
    cir.store %o, %slot : !cir.ptr<!s32i>, !cir.ptr<!cir.ptr<!s32i>>
    %loaded = cir.load %slot : !cir.ptr<!cir.ptr<!s32i>>, !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %loaded : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @main() {
    %q = cir.alloca "q" align(4) : !cir.ptr<!s32i>
    %p = cir.get_global @producer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%p, %q) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK: queue access is unresolvable
// CHECK-NOT: producers:
