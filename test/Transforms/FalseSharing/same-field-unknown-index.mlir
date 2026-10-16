// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two threads each increment counts[k] for a k of their own, which the
// analysis cannot read. On one element that is true sharing; on two
// neighbouring elements it is false sharing, and a per-thread counter in
// an int array is the textbook case. The same field of an array at an
// unknown index pairs with itself, as possible.

!s64i = !cir.int<s, 64>
!arr = !cir.array<!s64i x 16>
!fnptr = !cir.ptr<!cir.func<(!s64i)>>
module {
  cir.global external @counts = #cir.zero : !arr {alignment = 16 : i64}

  cir.func @spawn(%fn: !fnptr, %arg: !s64i) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @w(%k: !s64i) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @counts : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!s64i>
      %e = cir.ptr_stride %d, %k : (!cir.ptr<!s64i>, !s64i) -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %e : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %one = cir.const #cir.int<1> : !s64i
    %two = cir.const #cir.int<2> : !s64i
    %f = cir.get_global @w : !fnptr
    cir.call @spawn(%f, %one) : (!fnptr, !s64i) -> ()
    cir.call @spawn(%f, %two) : (!fnptr, !s64i) -> ()
    cir.return
  }
}

// CHECK-NOT: wtc: false sharing in
// CHECK: possible false sharing in @counts (element not known): '<element>' of two elements may share one 64-byte line and is touched repeatedly, but which element each is at the analysis cannot read
// CHECK: false sharing: 0 definite, 1 possible, 0 cold{{.*}}0 field accesses on objects that could not be identified{{.*}}1 at array elements the analysis cannot tell apart
