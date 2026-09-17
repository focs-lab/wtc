// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// The queue is bound at the spawn site, but the actual push happens several
// ordinary-call layers deeper: spawn(f1, &q) -> f1 calls f2 -> f2 calls f3
// -> f3 calls producer, which pushes. Neither f1, f2, f3, nor producer was
// ever itself passed directly to spawn -- only f1 was -- so both
// thread-context membership and queue-root resolution have to follow this
// chain of plain calls, not just the direct spawn binding. The consumer
// side uses a shorter (one-layer) chain to confirm the depth isn't
// hardcoded anywhere.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, %arg: !cir.ptr<!s32i>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @producer(%queue: !cir.ptr<!s32i>) {
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @f3(%queue: !cir.ptr<!s32i>) {
    cir.call @producer(%queue) : (!cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @f2(%queue: !cir.ptr<!s32i>) {
    cir.call @f3(%queue) : (!cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @f1(%queue: !cir.ptr<!s32i>) {
    cir.call @f2(%queue) : (!cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer(%queue: !cir.ptr<!s32i>) {
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @consumer_wrapper(%queue: !cir.ptr<!s32i>) {
    cir.call @consumer(%queue) : (!cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @main() {
    %q = cir.alloca "q" align(4) : !cir.ptr<!s32i>
    %p = cir.get_global @f1 : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%p, %q) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    %c = cir.get_global @consumer_wrapper : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%c, %q) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK-NOT: Could not resolve
// CHECK: Queue
// CHECK-NEXT: producers: 1
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: SPSC
