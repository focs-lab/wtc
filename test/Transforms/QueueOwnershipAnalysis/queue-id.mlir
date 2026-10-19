// RUN: wtc-opt %s --queue-ownership-analysis 2>/dev/null | FileCheck %s

// Each queue root gets a number, and every operation that can only act on
// that queue carries it, so a later pass can find all the operations of one
// queue. @drain pops whichever of the two queues it is handed: it acts on
// either, so it names neither.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @v : !s32i
  cir.global "private" external @a : !s32i
  cir.global "private" external @b : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, %arg: !cir.ptr<!s32i>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @drain(%queue: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %x = "wtc.queue_pop"(%h) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %v = cir.get_global @v : !cir.ptr<!s32i>
    %pa = cir.get_global @a : !cir.ptr<!s32i>
    %ha = builtin.unrealized_conversion_cast %pa : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%ha, %v) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    %pb = cir.get_global @b : !cir.ptr<!s32i>
    %hb = builtin.unrealized_conversion_cast %pb : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%hb, %v) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    %d = cir.get_global @drain : !cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>
    cir.call @spawn(%d, %pa) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.call @spawn(%d, %pb) : (!cir.ptr<!cir.func<(!cir.ptr<!s32i>)>>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK-DAG: cir.global "private" external @a : !s32i {wtc.queue_id = [[A:[0-9]+]] : i64,
// CHECK-DAG: cir.global "private" external @b : !s32i {wtc.queue_id = [[B:[0-9]+]] : i64,
// CHECK-LABEL: cir.func @drain
// CHECK: "wtc.queue_pop"
// CHECK-NOT: queue_id
// CHECK-SAME: -> !cir.ptr<!s32i>
// CHECK-LABEL: cir.func @main
// CHECK: "wtc.queue_push"({{.*}}) <{queue_id = [[A]] : i64,
// CHECK: "wtc.queue_push"({{.*}}) <{queue_id = [[B]] : i64,
