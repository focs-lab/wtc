// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// arr[k].cnt[j], with k and j each thread's own: two unknown steps, the
// inner over 8-byte longs inside a 128-byte element. Two such accesses may
// be 8 bytes apart, on one line, so the pair is possible; judging
// neighbours by the outer element only, 128 bytes apart, missed it.

!s64i = !cir.int<s, 64>
!cnt = !cir.array<!s64i x 16>
!rec_S = !cir.struct<"S" {data !cnt}>
!arr = !cir.array<!rec_S x 4>
!fnptr = !cir.ptr<!cir.func<(!s64i, !s64i)>>
module {
  cir.global external @arr = #cir.zero : !arr {alignment = 16 : i64}
  cir.func @spawn(%fn: !fnptr, %k: !s64i, %j: !s64i) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @w(%k: !s64i, %j: !s64i) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @arr : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %e = cir.ptr_stride %d, %k : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %c = cir.get_member %e[0] {name = "cnt"} : !cir.ptr<!rec_S> -> !cir.ptr<!cnt>
      %x = cir.get_element %c[%j : !s64i] : !cir.ptr<!cnt> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %x : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %zero = cir.const #cir.int<0> : !s64i
    %one = cir.const #cir.int<1> : !s64i
    %f = cir.get_global @w : !fnptr
    cir.call @spawn(%f, %zero, %zero) : (!fnptr, !s64i, !s64i) -> ()
    cir.call @spawn(%f, %zero, %one) : (!fnptr, !s64i, !s64i) -> ()
    cir.return
  }
}

// CHECK: possible false sharing in @arr (element not known): '[?].cnt[?]' of two elements may share one 64-byte line
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
