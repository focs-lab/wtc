// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// The shape every parameter and local pointer takes in real compiler output:
// the value is stored into a stack slot once and loaded wherever it is used.
// Without following that, the analysis resolves nothing at all from a real
// program, because block arguments only appear in hand-written IR.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @v = #cir.int<0> : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, %arg: !cir.ptr<!s32i>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @producer(%queue: !cir.ptr<!s32i>) {
    %slot = cir.alloca "q.addr" align(8) : !cir.ptr<!cir.ptr<!s32i>>
    cir.store %queue, %slot : !cir.ptr<!s32i>, !cir.ptr<!cir.ptr<!s32i>>
    %loaded = cir.load %slot : !cir.ptr<!cir.ptr<!s32i>>, !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %loaded : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer(%queue: !cir.ptr<!s32i>) {
    %slot = cir.alloca "q.addr" align(8) : !cir.ptr<!cir.ptr<!s32i>>
    cir.store %queue, %slot : !cir.ptr<!s32i>, !cir.ptr<!cir.ptr<!s32i>>
    %loaded = cir.load %slot : !cir.ptr<!cir.ptr<!s32i>>, !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %loaded : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %q = cir.alloca "q" align(4) : !cir.ptr<!s32i>
    %p = cir.get_global @producer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%p, %q) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%c, %q) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK: queue %q: spsc (producers: 1, consumers: 1)
