// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// worker_a stores arr[0].a once, then keeps storing arr[k].a for a k the
// analysis cannot read; worker_b hot-writes arr[0].b. As far as the
// constant-index accesses go, a is written once and the pair would be
// cold -- but worker_a's repeated store may be landing on arr[0].a. An
// access at an element the analysis cannot name blocks cold on that
// array, as an access it could not place at all does.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!arr = !cir.array<!rec_S x 4>
!fnptr = !cir.ptr<!cir.func<(!s64i)>>
module {
  cir.global external @arr = #cir.zero : !arr {alignment = 16 : i64}

  cir.func @spawn(%fn: !fnptr, %arg: !s64i) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @worker_a(%k: !s64i) {
    %p0 = cir.get_global @arr : !cir.ptr<!arr>
    %d0 = cir.cast array_to_ptrdecay %p0 : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
    %f0 = cir.get_member %d0[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %one = cir.const #cir.int<1> : !s64i
    cir.store %one, %f0 : !s64i, !cir.ptr<!s64i>
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @arr : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %e = cir.ptr_stride %d, %k : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %f = cir.get_member %e[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      cir.store %one, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @worker_b(%k: !s64i) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @arr : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %f = cir.get_member %d[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
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
    %a = cir.get_global @worker_a : !fnptr
    cir.call @spawn(%a, %one) : (!fnptr, !s64i) -> ()
    %b = cir.get_global @worker_b : !fnptr
    cir.call @spawn(%b, %one) : (!fnptr, !s64i) -> ()
    cir.return
  }
}

// CHECK-NOT: wtc: false sharing in
// CHECK: possible false sharing in @arr: 'a' and 'b'{{.*}}accesses it could not place
// CHECK: false sharing: 0 definite, 1 possible, 0 cold{{.*}}1 at array elements the analysis cannot tell apart{{.*}}1 of the possible pairs would be cold but for unplaced accesses
