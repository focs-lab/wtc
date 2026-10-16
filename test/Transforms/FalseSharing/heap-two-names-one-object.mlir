// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A wrapper allocates, registers the object in a global and also returns
// it, so the object reaches the workers under two names: the pointer main
// keeps and the global. The root is the allocator call behind both, so a
// hot write through one name and a hot read through the other are one
// pair. With the wrapper's call as the root of the returned name, the two
// names were two roots and the pair on one of them came out cold.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!u8i = !cir.int<u, 8>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @tp = #cir.ptr<null> : !cir.ptr<!rec_S>
  cir.global external @reg = #cir.ptr<null> : !cir.ptr<!rec_S>
  cir.func private @calloc(!u64i, !u64i) -> !cir.ptr<!void>
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @xcalloc(%n: !u64i, %m: !u64i) -> !cir.ptr<!void> {
    %r = cir.call @calloc(%n, %m) : (!u64i, !u64i) -> !cir.ptr<!void>
    %rs = cir.cast bitcast %r : !cir.ptr<!void> -> !cir.ptr<!rec_S>
    %rp = cir.get_global @reg : !cir.ptr<!cir.ptr<!rec_S>>
    cir.store %rs, %rp : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    cir.return %r : !cir.ptr<!void>
  }
  cir.func internal private @wa() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
      %p = cir.load %tpp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %f = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.load %f : !cir.ptr<!s64i>, !s64i
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @wb() {
    %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
    %p = cir.load %tpp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
    %f = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %f : !s64i, !cir.ptr<!s64i>
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %rp = cir.get_global @reg : !cir.ptr<!cir.ptr<!rec_S>>
      %q = cir.load %rp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %g = cir.get_member %q[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %w = cir.const #cir.int<2> : !s64i
      cir.store %w, %g : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %one = cir.const #cir.int<1> : !u64i
    %sz = cir.const #cir.int<16> : !u64i
    %r = cir.call @xcalloc(%one, %sz) : (!u64i, !u64i) -> !cir.ptr<!void>
    %rs = cir.cast bitcast %r : !cir.ptr<!void> -> !cir.ptr<!rec_S>
    %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
    cir.store %rs, %tpp : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in calloc() in @xcalloc: 'a' (offset 0) and 'b' (offset 8) are touched by different threads
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
