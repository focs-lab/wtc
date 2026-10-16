// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// One element per thread: main allocates an array of 40-byte elements and
// hands each thread the element at the spawn loop's own index. Each thread
// hot-writes 'a' and 'e' of its element. Within an element the two fields
// belong to one thread, so no pair; across the element boundary 'e' of
// element i and 'a' of element i+1 belong to two threads and share a line
// in 7 of the 8 places a 40-byte element of a 16-aligned array may start
// (only an element ending on a line boundary keeps them apart). That is the
// Phoenix linear_regression shape: a results struct per thread, in a
// calloc'd array, straddling lines.

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
    cir.return
  }
}

// CHECK-NOT: in @main (one element per thread): 'a'
// CHECK: wtc: false sharing in calloc() in @main (one element per thread) across elements: 'e' (offset 32) of one element and 'a' (offset 0) of the next are touched by the two threads that own them and share one 64-byte line in 7 of the 8 positions
// CHECK: false sharing: {{[1-9]}} definite, 0 possible, 0 cold{{.*}}0 field accesses on objects that could not be identified
