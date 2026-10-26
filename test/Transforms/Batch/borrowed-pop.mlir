// RUN: wtc-opt %s --lift-cir-to-wtc --wtc-batch-pushes 2>&1 >/dev/null | FileCheck %s

// The shared_ptr a borrowed pop of an inline queue returns owns nothing,
// and its destructor frees nothing: a loop of pushes that destroys one is
// still fit to batch (the shim's batch functions are missing here, which is
// all that stops it). The same destructor on the shared_ptr of any other
// pop is a call into a function whose body is not here.
//
// The pops are written as the specialisation leaves them; in wtc-cc's order
// batching runs before it, and no pop is inline yet.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec = !cir.struct<"record" {data !s64i}>
!sp = !cir.struct<"std::shared_ptr<record>" {data !cir.ptr<!rec>, data !cir.ptr<!s64i>}>
!q = !cir.struct<"wtc::queue<record>" {data !s64i}>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @push(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push">]
  cir.func private @_ZNSt10shared_ptrI6recordED1Ev(!cir.ptr<!sp>)
  cir.global external @in = #cir.zero : !q
  cir.global external @out = #cir.zero : !queue_i32
  cir.global external @v = #cir.int<1> : !s32i

  cir.func @borrowed() [#cir.annotation<"wtc_batch">] {
    %p = cir.alloca "p" align(8) init : !cir.ptr<!sp>
    %g = cir.get_global @in : !cir.ptr<!q>
    %h = builtin.unrealized_conversion_cast %g : !cir.ptr<!q> to !wtc.queue<!q>
    "wtc.queue_pop"(%h, %p) {queue_id = 0 : i64, impl = #wtc.impl<spsc_inline>, borrowed} : (!wtc.queue<!q>, !cir.ptr<!sp>) -> ()
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %o = cir.get_global @out : !cir.ptr<!queue_i32>
      %v = cir.get_global @v : !cir.ptr<!s32i>
      cir.call @push(%o, %v) : (!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> ()
      cir.call @_ZNSt10shared_ptrI6recordED1Ev(%p) : (!cir.ptr<!sp>) -> ()
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }

  cir.func @owned() [#cir.annotation<"wtc_batch">] {
    %p = cir.alloca "p" align(8) init : !cir.ptr<!sp>
    %g = cir.get_global @in : !cir.ptr<!q>
    %h = builtin.unrealized_conversion_cast %g : !cir.ptr<!q> to !wtc.queue<!q>
    "wtc.queue_pop"(%h, %p) {queue_id = 0 : i64, impl = #wtc.impl<spsc>} : (!wtc.queue<!q>, !cir.ptr<!sp>) -> ()
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %o = cir.get_global @out : !cir.ptr<!queue_i32>
      %v = cir.get_global @v : !cir.ptr<!s32i>
      cir.call @push(%o, %v) : (!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> ()
      cir.call @_ZNSt10shared_ptrI6recordED1Ev(%p) : (!cir.ptr<!sp>) -> ()
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
}

// CHECK-DAG: remark: wtc: pushes in this loop not batched: the shim's batch functions for this queue are not in the module
// CHECK-DAG: remark: wtc: pushes in this loop not batched: it calls std::shared_ptr<record>::~shared_ptr(), which may throw
