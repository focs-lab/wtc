// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// One memcpy reads an element the analysis cannot name and writes through
// a pointer it cannot place: one operation, counted once in the summary,
// under the least that is known of it. It used to be counted in both
// clauses.

!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!arr = !cir.array<!rec_S x 4>
!fnptr = !cir.ptr<!cir.func<(!s64i)>>
module {
  cir.global external @arr = #cir.zero : !arr {alignment = 16 : i64}
  cir.global external @dst = #cir.ptr<null> : !cir.ptr<!rec_S>
  cir.global external @other = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr, %arg: !s64i) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @w(%k: !s64i) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @arr : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %e = cir.ptr_stride %d, %k : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %f = cir.get_member %e[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %fv = cir.cast bitcast %f : !cir.ptr<!s64i> -> !cir.ptr<!void>
      %dp = cir.get_global @dst : !cir.ptr<!cir.ptr<!rec_S>>
      %q = cir.load %dp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %qa = cir.get_member %q[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %qv = cir.cast bitcast %qa : !cir.ptr<!s64i> -> !cir.ptr<!void>
      %n = cir.const #cir.int<8> : !u64i
      cir.libc.memcpy %n bytes from %fv to %qv : !u64i, !cir.ptr<!void> -> !cir.ptr<!void>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %o = cir.get_global @other : !cir.ptr<!rec_S>
    %dp = cir.get_global @dst : !cir.ptr<!cir.ptr<!rec_S>>
    cir.store %o, %dp : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    cir.store %o, %dp : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %one = cir.const #cir.int<1> : !s64i
    %f = cir.get_global @w : !fnptr
    cir.call @spawn(%f, %one) : (!fnptr, !s64i) -> ()
    cir.return
  }
}

// CHECK: false sharing: 0 definite, 0 possible, 0 cold{{.*}}, 1 field accesses on objects that could not be identified{{.*}}, 0 at array elements the analysis cannot tell apart
