// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The array is allocated in a loop, so the root stands for many objects;
// two threads touching elements the analysis cannot name may be on two
// objects altogether. The pair is still said, as possible, with the
// reason; it used to be dropped without a trace, and the summary counted
// the accesses as placed and reported nothing.

!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<(!s64i)>>
module {
  cir.global external @tp = #cir.ptr<null> : !cir.ptr<!rec_S>
  cir.func private @calloc(!u64i, !u64i) -> !cir.ptr<!void>
  cir.func @spawn(%fn: !fnptr, %arg: !s64i) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa(%k: !s64i) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
      %p = cir.load %tpp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %e = cir.ptr_stride %p, %k : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %f = cir.get_member %e[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @wb(%k: !s64i) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
      %p = cir.load %tpp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %e = cir.ptr_stride %p, %k : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %f = cir.get_member %e[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @init() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %n = cir.const #cir.int<4> : !u64i
      %sz = cir.const #cir.int<16> : !u64i
      %r = cir.call @calloc(%n, %sz) : (!u64i, !u64i) -> !cir.ptr<!void>
      %rs = cir.cast bitcast %r : !cir.ptr<!void> -> !cir.ptr<!rec_S>
      %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
      cir.store %rs, %tpp : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    cir.call @init() : () -> ()
    %one = cir.const #cir.int<1> : !s64i
    %two = cir.const #cir.int<2> : !s64i
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a, %one) : (!fnptr, !s64i) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b, %two) : (!fnptr, !s64i) -> ()
    cir.return
  }
}

// CHECK: possible false sharing in calloc() in @init (element not known): '[?].a' and '[?].b' share one 64-byte line, but the allocation may run more than once
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
