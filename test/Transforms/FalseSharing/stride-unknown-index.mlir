// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two threads write two fields of elements of one global array, each at
// an index handed to it as its argument, which the analysis cannot read.
// The accesses are on a known array all the same: they used to be counted
// as unplaced, and now the pair is reported -- as possible, never
// definite, because the two may be on one element or on two.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!arr = !cir.array<!rec_S x 4>
!fnptr = !cir.ptr<!cir.func<(!s64i)>>
module {
  cir.global external @arr = #cir.zero : !arr {alignment = 16 : i64}

  cir.func @spawn(%fn: !fnptr, %arg: !s64i) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa(%k: !s64i) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @arr : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %e = cir.ptr_stride %d, %k : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %f = cir.get_member %e[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @wb(%k: !s64i) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @arr : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %e = cir.ptr_stride %d, %k : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %f = cir.get_member %e[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %one = cir.const #cir.int<1> : !s64i
    %two = cir.const #cir.int<2> : !s64i
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a, %one) : (!fnptr, !s64i) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b, %two) : (!fnptr, !s64i) -> ()
    cir.return
  }
}

// CHECK-NOT: wtc: false sharing in
// CHECK: possible false sharing in @arr (element not known): '[?].a' and '[?].b' share one 64-byte line and are touched repeatedly, but which element each is at the analysis cannot read, so the two may be on one element or on two
// CHECK: false sharing: 0 definite, 1 possible, 0 cold{{.*}}, 0 field accesses on objects that could not be identified{{.*}}, 2 at array elements the analysis cannot tell apart
