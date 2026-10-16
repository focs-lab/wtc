!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
!arr = !cir.array<!rec_S x 4>
// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A global array of structs. One thread writes arr[1].a, at offset 16, the
// other arr[0].b, at offset 8: two elements, one line, definite. A step
// through an element with a constant index adds that many elements to the
// offset; the access used to stop at the stride as unplaced.

module {
  cir.global external @arr = #cir.zero : !arr {alignment = 16 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %g = cir.get_global @arr : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %g : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %i = cir.const #cir.int<1> : !s64i
      %e = cir.ptr_stride %d, %i : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %f = cir.get_member %e[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @wb() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %g = cir.get_global @arr : !cir.ptr<!arr>
      %i = cir.const #cir.int<0> : !s64i
      %e = cir.get_element %g[%i : !s64i] : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
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
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in @arr: 'b' (offset 8) and '[1].a' (offset 16)
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
