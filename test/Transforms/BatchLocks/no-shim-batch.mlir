// RUN: wtc-opt %s --lift-cir-to-wtc --wtc-batch-locks 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// A loop of critical sections that qualifies for holding the mutex across
// them, in a module that holds no batch functions for the mutex's type:
// the shim's batch_begin, batch_step and batch_end are what the rewrite
// calls, and when the module was compiled without them there is nothing to
// rewrite to. The pass says so and leaves the loop alone.

!s32i = !cir.int<s, 32>
!mutex = !cir.struct<"wtc::mutex" {data !s32i}>
module {
  cir.func private @lock(!cir.ptr<!mutex>) [#cir.annotation<"wtc_mutex_lock">]
  cir.func private @unlock(!cir.ptr<!mutex>) [#cir.annotation<"wtc_mutex_unlock">]
  cir.global external @m = #cir.zero : !mutex
  cir.global external @total = #cir.int<0> : !s32i

  cir.func @add() [#cir.annotation<"wtc_batch">] {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %m = cir.get_global @m : !cir.ptr<!mutex>
      cir.call @lock(%m) : (!cir.ptr<!mutex>) -> ()
      %p = cir.get_global @total : !cir.ptr<!s32i>
      %one = cir.const #cir.int<1> : !s32i
      cir.store %one, %p : !s32i, !cir.ptr<!s32i>
      cir.call @unlock(%m) : (!cir.ptr<!mutex>) -> ()
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
}

// CHECK: remark: wtc: critical sections in this loop not batched: the shim's batch functions for this mutex are not in the module
// IR-NOT: lock_batch
// IR: "wtc.lock"
