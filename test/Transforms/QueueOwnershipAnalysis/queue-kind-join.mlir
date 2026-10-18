// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// One push op in a helper reaches two queues: thread A calls it with @q1,
// threads B and C with @q2. @q1 is SPSC and @q2 is MPSC, and each global
// carries its own verdict. The shared push op carries the join, MPSC, since
// a substitution keyed off that op would have to be right for both.

!s32i = !cir.int<s, 32>
!fn = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @q1 : !s32i
  cir.global "private" external @q2 : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !fn) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @push_to(%queue: !cir.ptr<!s32i>) {
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @a() {
    %q = cir.get_global @q1 : !cir.ptr<!s32i>
    cir.call @push_to(%q) : (!cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @b() {
    %q = cir.get_global @q2 : !cir.ptr<!s32i>
    cir.call @push_to(%q) : (!cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @pop1() {
    %queue = cir.get_global @q1 : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @pop2() {
    %queue = cir.get_global @q2 : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %a = cir.get_global @a : !fn
    cir.call @spawn(%a) : (!fn) -> ()
    %b = cir.get_global @b : !fn
    cir.call @spawn(%b) : (!fn) -> ()
    cir.call @spawn(%b) : (!fn) -> ()
    %p1 = cir.get_global @pop1 : !fn
    cir.call @spawn(%p1) : (!fn) -> ()
    %p2 = cir.get_global @pop2 : !fn
    cir.call @spawn(%p2) : (!fn) -> ()
    cir.return
  }
}

// CHECK-LABEL: Queue @q1
// CHECK: classification: SPSC
// CHECK-LABEL: Queue @q2
// CHECK: classification: MPSC

// CHECK-DAG: cir.global "private" external @q1 {{.*}}wtc.queue_kind = "SPSC"
// CHECK-DAG: cir.global "private" external @q2 {{.*}}wtc.queue_kind = "MPSC"

// CHECK-LABEL: cir.func @push_to
// CHECK: "wtc.queue_push"{{.*}}wtc.queue_kind = "MPSC"
// CHECK-LABEL: cir.func @pop1
// CHECK: "wtc.queue_pop"{{.*}}wtc.queue_kind = "SPSC"
// CHECK-LABEL: cir.func @pop2
// CHECK: "wtc.queue_pop"{{.*}}wtc.queue_kind = "MPSC"
