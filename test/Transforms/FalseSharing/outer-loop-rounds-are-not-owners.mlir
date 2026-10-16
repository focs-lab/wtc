// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The spawn loop sits inside another loop: each round hands element i to
// a new thread, so a thread does not own its element alone, and the two
// fields it touches are possible -- one thread standing for many, which
// may be two rounds' threads on one element -- and nothing is claimed
// across elements.

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
      %b = cir.get_member %me[1] {name = "b"} : !cir.ptr<!rec_W> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %a : !s64i, !cir.ptr<!s64i>
      cir.store %v, %b : !s64i, !cir.ptr<!s64i>
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
    %sz = cir.const #cir.int<64> : !u64i
    %r = cir.call @calloc(%n, %sz) : (!u64i, !u64i) -> !cir.ptr<!void>
    %rs = cir.cast bitcast %r : !cir.ptr<!void> -> !cir.ptr<!rec_W>
    cir.store %rs, %slot : !cir.ptr<!rec_W>, !cir.ptr<!cir.ptr<!rec_W>>
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
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
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
}

// CHECK-NOT: across elements
// CHECK: possible false sharing in calloc() in @main (one element per thread): 'a' and 'b'{{.*}}one thread that stands for many
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
