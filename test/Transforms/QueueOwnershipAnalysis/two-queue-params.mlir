// RUN: wtc-opt %s --queue-ownership-analysis | FileCheck %s

// A pipeline stage takes two queues, pops from its first parameter and
// pushes to its second. Resolving a parameter used to check only that it
// belonged to the function and then take argument zero of the call, so
// every access to the second queue was attributed to the first. Each
// queue here has exactly one producer and one consumer.

!s32i = !cir.int<s, 32>
!fn = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @in : !s32i
  cir.global "private" external @out : !s32i
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !fn) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @relay(%from: !cir.ptr<!s32i>, %to: !cir.ptr<!s32i>) {
    %fcast = builtin.unrealized_conversion_cast %from : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%fcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    %tcast = builtin.unrealized_conversion_cast %to : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%tcast, %popped) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @stage() {
    %in = cir.get_global @in : !cir.ptr<!s32i>
    %out = cir.get_global @out : !cir.ptr<!s32i>
    cir.call @relay(%in, %out) : (!cir.ptr<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @source() {
    %queue = cir.get_global @in : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @sink() {
    %queue = cir.get_global @out : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %src = cir.get_global @source : !fn
    cir.call @spawn(%src) : (!fn) -> ()
    %stg = cir.get_global @stage : !fn
    cir.call @spawn(%stg) : (!fn) -> ()
    %snk = cir.get_global @sink : !fn
    cir.call @spawn(%snk) : (!fn) -> ()
    cir.return
  }
}

// CHECK-LABEL: Queue @out
// CHECK-NEXT: producers: 1
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: SPSC
// CHECK-LABEL: Queue @in
// CHECK-NEXT: producers: 1
// CHECK-NEXT: consumers: 1
// CHECK-NEXT: classification: SPSC
