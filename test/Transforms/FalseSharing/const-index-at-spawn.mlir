// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The spawns hand out &base[1] and &base[0] directly: a constant index at
// the spawn is so many bytes into the array, and e of element 0 and a of
// element 1 are adjacent, a definite pair. The step used to be refused
// unless its index was the loop's variable.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_W = !cir.struct<"W" {data !s64i, data !s64i, data !s64i, data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<(!cir.ptr<!rec_W>)>>
module {
  cir.func private @calloc(!u64i, !u64i) -> !cir.ptr<!void>
  cir.func @spawn(%fn: !fnptr, %arg: !cir.ptr<!rec_W>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa(%me: !cir.ptr<!rec_W>) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %a = cir.get_member %me[0] {name = "a"} : !cir.ptr<!rec_W> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %a : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @we(%me: !cir.ptr<!rec_W>) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %e = cir.get_member %me[4] {name = "e"} : !cir.ptr<!rec_W> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %e : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %n = cir.const #cir.int<8> : !u64i
    %sz = cir.const #cir.int<40> : !u64i
    %r = cir.call @calloc(%n, %sz) : (!u64i, !u64i) -> !cir.ptr<!void>
    %base = cir.cast bitcast %r : !cir.ptr<!void> -> !cir.ptr<!rec_W>
    %k0 = cir.const #cir.int<0> : !s64i
    %k1 = cir.const #cir.int<1> : !s64i
    %e0 = cir.ptr_stride %base, %k0 : (!cir.ptr<!rec_W>, !s64i) -> !cir.ptr<!rec_W>
    %e1 = cir.ptr_stride %base, %k1 : (!cir.ptr<!rec_W>, !s64i) -> !cir.ptr<!rec_W>
    %fa = cir.get_global @wa : !fnptr
    cir.call @spawn(%fa, %e1) : (!fnptr, !cir.ptr<!rec_W>) -> ()
    %fe = cir.get_global @we : !fnptr
    cir.call @spawn(%fe, %e0) : (!fnptr, !cir.ptr<!rec_W>) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in calloc() in @main: 'e' (offset 32) and 'a' (offset 40) are touched by different threads
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
