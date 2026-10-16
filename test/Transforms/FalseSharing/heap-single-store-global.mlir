// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// main allocates one struct and stores the pointer into a global the module
// writes nowhere else; two workers load the pointer and each writes one
// field in a loop. The object is "the one calloc in main allocates", and
// the pair is definite. The analysis used to stop at the load from the
// global, so every access through a heap pointer was one it could not
// place -- which on memcached was its whole worker pool, threads = calloc(...).

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @tp = #cir.ptr<null> : !cir.ptr<!rec_S>
  cir.func private @calloc(!u64i, !u64i) -> !cir.ptr<!void>
  cir.func private @malloc(!u64i) -> !cir.ptr<!void>
  cir.func private @realloc(!cir.ptr<!void>, !u64i) -> !cir.ptr<!void>
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
      %p = cir.load %tpp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %f = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @wb() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
      %p = cir.load %tpp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %f = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %one = cir.const #cir.int<1> : !u64i
    %sz = cir.const #cir.int<16> : !u64i
    %r = cir.call @calloc(%one, %sz) : (!u64i, !u64i) -> !cir.ptr<!void>
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

// CHECK: wtc: false sharing in calloc() in @main: 'a' (offset 0) and 'b' (offset 8)
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
