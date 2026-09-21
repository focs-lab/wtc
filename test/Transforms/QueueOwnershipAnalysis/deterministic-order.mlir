// RUN: wtc-opt %s --queue-ownership-analysis 2>%t.a >/dev/null
// RUN: wtc-opt %s --queue-ownership-analysis 2>%t.b >/dev/null
// RUN: diff %t.a %t.b
// RUN: FileCheck %s < %t.a

// Four queues, and the verdicts must come out in the same order every time.
// They used to be held in a hash map, so the order was whatever the
// addresses happened to give, and a test matching them in any order would
// have accepted the verdicts attached to the wrong queues. Running the pass
// twice and comparing is the part that actually tests determinism; the
// ordered matching below tests that the order is the module's.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @qa : !s32i
  cir.global "private" external @qb : !s32i
  cir.global "private" external @qc : !s32i
  cir.global "private" external @qd : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @w() {
    %a = cir.get_global @qa : !cir.ptr<!s32i>
    %b = cir.get_global @qb : !cir.ptr<!s32i>
    %c = cir.get_global @qc : !cir.ptr<!s32i>
    %d = cir.get_global @qd : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %ca = builtin.unrealized_conversion_cast %a : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %cb = builtin.unrealized_conversion_cast %b : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %cc = builtin.unrealized_conversion_cast %c : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %cd = builtin.unrealized_conversion_cast %d : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%ca, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    "wtc.queue_push"(%cb, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    "wtc.queue_push"(%cc, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    "wtc.queue_push"(%cd, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @main() {
    %f = cir.get_global @w : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%f) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// CHECK: queue @qa:
// CHECK: queue @qb:
// CHECK: queue @qc:
// CHECK: queue @qd:
