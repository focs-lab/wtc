// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two spawn loops over one array: the first hands element i to a thread
// that hot-writes a, the second to a thread that hot-writes e. The two
// threads of element i are different threads on one element, a definite
// pair within the element; the places used to be keyed by the spawn loop,
// and the two never met.

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
    %i = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %n = cir.const #cir.int<8> : !u64i
    %sz = cir.const #cir.int<40> : !u64i
    %r = cir.call @calloc(%n, %sz) : (!u64i, !u64i) -> !cir.ptr<!void>
    %base = cir.cast bitcast %r : !cir.ptr<!void> -> !cir.ptr<!rec_W>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %i : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<8> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %k = cir.cast integral %c : !s32i -> !s64i
      %elem = cir.ptr_stride %base, %k : (!cir.ptr<!rec_W>, !s64i) -> !cir.ptr<!rec_W>
      %f = cir.get_global @wa : !fnptr
      cir.call @spawn(%f, %elem) : (!fnptr, !cir.ptr<!rec_W>) -> ()
      cir.yield
    } step {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %inc = cir.inc nsw %c : !s32i
      cir.store %inc, %i : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.store %z, %i : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<8> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %k = cir.cast integral %c : !s32i -> !s64i
      %elem = cir.ptr_stride %base, %k : (!cir.ptr<!rec_W>, !s64i) -> !cir.ptr<!rec_W>
      %f = cir.get_global @we : !fnptr
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

// CHECK: wtc: false sharing in calloc() in @main (one element per thread): 'a' (offset 0) and 'e' (offset 32) are touched by different threads and share one 64-byte line in 4 of the 8 positions an element of 40 bytes may start at
// CHECK: false sharing: 3 definite, 0 possible, 0 cold
