// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// The shape the compiler emits for spawn. It returns a thread object by
// value, so the call gets a hidden first pointer argument and the entry
// function is operand one. Reading operand zero finds the return slot, no
// thread is ever identified, and the analysis reports nothing at all about a
// program full of threads, which is what it did on the first real example.

!s32i = !cir.int<s, 32>
!thread = !cir.struct<"std::thread" {data !cir.ptr<!s32i>}>
module {
  cir.global "private" external @q = #cir.int<0> : !s32i
  cir.global "private" external @v = #cir.int<0> : !s32i

  cir.func @spawn(%ret: !cir.ptr<!thread>, %fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @producer() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @main() {
    %t1 = cir.alloca "t1" align(8) : !cir.ptr<!thread>
    %t2 = cir.alloca "t2" align(8) : !cir.ptr<!thread>
    %p = cir.get_global @producer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%t1, %p) : (!cir.ptr<!thread> {llvm.sret = !thread}, !cir.ptr<!cir.func<()>>) -> ()
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%t2, %c) : (!cir.ptr<!thread> {llvm.sret = !thread}, !cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// CHECK: queue @q: spsc (producers: 1, consumers: 1)
