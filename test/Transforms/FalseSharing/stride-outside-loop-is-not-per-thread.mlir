// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The element address is computed before the loop, so every thread the
// loop starts gets element 0: not one element per thread. The handle is
// at an element the analysis does not read, and the two fields are
// possible; the per-thread reading used to apply, with its claims across
// elements, because only where the index was stored was checked.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_W = !cir.struct<"W" {data !s64i, data !s64i, data !s64i, data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<(!cir.ptr<!rec_W>)>>
module {
  cir.func private @calloc(!u64i, !u64i) -> !cir.ptr<!void>
  cir.func @spawn(%fn: !fnptr, %arg: !cir.ptr<!rec_W>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @worker(%me: !cir.ptr<!rec_W>) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %a = cir.get_member %me[0] {name = "a"} : !cir.ptr<!rec_W> -> !cir.ptr<!s64i>
      %e = cir.get_member %me[4] {name = "e"} : !cir.ptr<!rec_W> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %a : !s64i, !cir.ptr<!s64i>
      cir.store %v, %e : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %i = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %n = cir.const #cir.int<8> : !u64i
    %sz = cir.const #cir.int<40> : !u64i
    %r = cir.call @calloc(%n, %sz) : (!u64i, !u64i) -> !cir.ptr<!void>
    %base = cir.cast bitcast %r : !cir.ptr<!void> -> !cir.ptr<!rec_W>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %i : !s32i, !cir.ptr<!s32i>
    %c0 = cir.load %i : !cir.ptr<!s32i>, !s32i
    %k0 = cir.cast integral %c0 : !s32i -> !s64i
    %elem = cir.ptr_stride %base, %k0 : (!cir.ptr<!rec_W>, !s64i) -> !cir.ptr<!rec_W>
    cir.store %z, %i : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<8> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %f = cir.get_global @worker : !fnptr
      cir.call @spawn(%f, %elem) : (!fnptr, !cir.ptr<!rec_W>) -> ()
      cir.yield
    } step {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %inc = cir.inc nsw %c : !s32i
      cir.store %inc, %i : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
}

// CHECK-NOT: across elements
// CHECK: possible false sharing in calloc() in @main (element not known): 'a' and 'e'
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
