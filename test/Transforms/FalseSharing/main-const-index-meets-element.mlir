// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Workers hot-write a of their own element; main, after the spawn loop,
// hot-reads c of element 0 by a constant index. An access at a constant
// index is in some element, at the offset modulo the element size, where
// it meets the per-thread accesses: main and thread 0 on one element, a
// definite pair. It used to be placed by its absolute offset, where it met
// nothing.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!u8i = !cir.int<u, 8>
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
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %a : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %slot = cir.alloca "w" align(8) : !cir.ptr<!cir.ptr<!rec_W>>
    %i = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %n = cir.const #cir.int<8> : !u64i
    %sz = cir.const #cir.int<40> : !u64i
    %r = cir.call @calloc(%n, %sz) : (!u64i, !u64i) -> !cir.ptr<!void>
    %rs = cir.cast bitcast %r : !cir.ptr<!void> -> !cir.ptr<!rec_W>
    cir.store %rs, %slot : !cir.ptr<!rec_W>, !cir.ptr<!cir.ptr<!rec_W>>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %i : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<8> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %base = cir.load %slot : !cir.ptr<!cir.ptr<!rec_W>>, !cir.ptr<!rec_W>
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %k = cir.cast integral %c : !s32i -> !s64i
      %elem = cir.ptr_stride %base, %k : (!cir.ptr<!rec_W>, !s64i) -> !cir.ptr<!rec_W>
      %f = cir.get_global @worker : !fnptr
      cir.call @spawn(%f, %elem) : (!fnptr, !cir.ptr<!rec_W>) -> ()
      cir.yield
    } step {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %inc = cir.inc nsw %c : !s32i
      cir.store %inc, %i : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %base = cir.load %slot : !cir.ptr<!cir.ptr<!rec_W>>, !cir.ptr<!rec_W>
      %cc = cir.get_member %base[2] {name = "c"} : !cir.ptr<!rec_W> -> !cir.ptr<!s64i>
      %x = cir.load %cc : !cir.ptr<!s64i>, !s64i
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
}

// CHECK: wtc: false sharing in calloc() in @main (one element per thread): 'a' (offset 0) and 'c' (offset 16) are touched by different threads
// CHECK: false sharing: 2 definite, 0 possible, 0 cold
