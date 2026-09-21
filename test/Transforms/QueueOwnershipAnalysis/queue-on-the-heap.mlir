// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// A queue allocated on the heap. Tracing ends at the call that returned the
// pointer, and one call site can hand out any number of distinct objects, so
// there is no object to attribute the access to. Unknown, and the
// general-purpose implementation stays.

!s32i = !cir.int<s, 32>
!u64i = !cir.int<u, 64>
module {
  cir.global "private" external @v : !s32i
  cir.func private @_Znwm(!u64i) -> !cir.ptr<!s32i>

  cir.func @spawn(%fn: !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, %arg: !cir.ptr<!s32i>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @producer(%queue: !cir.ptr<!s32i>) {
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @main() {
    %sz = cir.const #cir.int<8> : !u64i
    %q = cir.call @_Znwm(%sz) : (!u64i) -> !cir.ptr<!s32i>
    %p = cir.get_global @producer : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%p, %q) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK: queue access is unresolvable
// CHECK-NOT: producers:
