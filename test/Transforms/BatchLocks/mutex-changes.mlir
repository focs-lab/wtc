// RUN: wtc-opt %s --lift-cir-to-wtc --wtc-batch-locks 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// The lock and the unlock take one handle, but the handle is read inside
// the loop, from a pointer the loop may see change: which mutex the next
// iteration takes is not the one this iteration took, and holding it
// across the iterations would hold the wrong one. From C++ the front end
// reloads the handle for each call, so the two differ and are refused
// as different mutexes; this is the shape where they agree.

!s32i = !cir.int<s, 32>
!mutex = !cir.struct<"wtc::mutex" {data !s32i}>
module {
  cir.func private @lock(!cir.ptr<!mutex>) [#cir.annotation<"wtc_mutex_lock">]
  cir.func private @unlock(!cir.ptr<!mutex>) [#cir.annotation<"wtc_mutex_unlock">]
  cir.global external @current = #cir.ptr<null> : !cir.ptr<!mutex>
  cir.global external @total = #cir.int<0> : !s32i

  cir.func @add() [#cir.annotation<"wtc_batch">] {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %pp = cir.get_global @current : !cir.ptr<!cir.ptr<!mutex>>
      %m = cir.load %pp : !cir.ptr<!cir.ptr<!mutex>>, !cir.ptr<!mutex>
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

// CHECK: remark: wtc: critical sections in this loop not batched: the loop may change which mutex it takes
// IR-NOT: lock_batch
