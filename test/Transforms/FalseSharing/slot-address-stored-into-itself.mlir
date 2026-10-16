// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A worker keeps the address of its own local array in one of the array's
// elements and hands the element to a call. The address escapes. The escape
// check met the store first as the element's user, remembered the store as
// seen, and skipped it when it met it again as the address's user, so the
// slot passed as private and its pair came out cold.

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
  cir.func private @sink(!cir.ptr<!void>)
  cir.func internal private @wa() {
    %buf = cir.alloca "buf" align(8) : !cir.ptr<!cir.array<!cir.ptr<!void> x 4>>
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %one = cir.const #cir.int<1> : !s64i
    cir.store %one, %a : !s64i, !cir.ptr<!s64i>
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %i = cir.const #cir.int<1> : !s64i
      %e = cir.get_element %buf[%i : !s64i] : !cir.ptr<!cir.array<!cir.ptr<!void> x 4>> -> !cir.ptr<!cir.ptr<!void>>
      %c = cir.cast bitcast %buf : !cir.ptr<!cir.array<!cir.ptr<!void> x 4>> -> !cir.ptr<!void>
      cir.store %c, %e : !cir.ptr<!void>, !cir.ptr<!cir.ptr<!void>>
      %l = cir.load %e : !cir.ptr<!cir.ptr<!void>>, !cir.ptr<!void>
      cir.call @sink(%l) : (!cir.ptr<!void>) -> ()
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

// CHECK: possible false sharing in @s: 'a' and 'b'{{.*}}accesses it could not place
// CHECK: false sharing: 0 definite, 1 possible, 0 cold{{.*}}0 on stack slots that never escape
