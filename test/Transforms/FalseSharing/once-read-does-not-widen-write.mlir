// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// main starts the worker that writes b, reads the whole struct once, writes
// a once, and then keeps writing an element of a global array at an index
// it loads, which the walk cannot place. The pair of a and b is possible:
// main's thread has unplaced accesses. It vanished once, because the
// 16-byte once-only read widened main's once-only write of a over b.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wb() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %f = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.global external @arr = #cir.zero : !cir.array<!rec_S x 4> {alignment = 16 : i64}
  cir.global external @idx = #cir.int<1> : !s64i {alignment = 8 : i64}
  cir.func @main() {
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %snap = cir.load %p : !cir.ptr<!rec_S>, !rec_S
    %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %one = cir.const #cir.int<1> : !s64i
    cir.store %one, %a : !s64i, !cir.ptr<!s64i>
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %g = cir.get_global @arr : !cir.ptr<!cir.array<!rec_S x 4>>
      %ip = cir.get_global @idx : !cir.ptr<!s64i>
      %i = cir.load %ip : !cir.ptr<!s64i>, !s64i
      %e = cir.get_element %g[%i : !s64i] : !cir.ptr<!cir.array<!rec_S x 4>> -> !cir.ptr<!rec_S>
      %f = cir.get_member %e[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      cir.store %one, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
}

// CHECK: possible false sharing in @s: 'a' and 'b'{{.*}}accesses it could not place
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
