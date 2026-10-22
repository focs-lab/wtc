// RUN: wtc-opt %s --lift-cir-to-wtc --wtc-batch-pushes 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// A loop of pushes that qualifies for batching, in a module that holds no
// batch functions for the queue's type: the shim's batch_begin,
// push_deferred and batch_end are what the rewrite calls, and when the
// module was compiled without them there is nothing to rewrite to. The
// pass says so and leaves the loop alone.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @push(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push">]
  cir.global external @q = #cir.zero : !queue_i32
  cir.global external @v = #cir.int<1> : !s32i

  cir.func @fill() [#cir.annotation<"wtc_batch">] {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %q = cir.get_global @q : !cir.ptr<!queue_i32>
      %v = cir.get_global @v : !cir.ptr<!s32i>
      cir.call @push(%q, %v) : (!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> ()
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
}

// CHECK: remark: wtc: pushes in this loop not batched: the shim's batch functions for this queue are not in the module
// IR-NOT: wtc.queue_push_deferred
// IR: "wtc.queue_push"
