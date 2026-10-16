// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The allocation comes through a checked wrapper, the way Phoenix's CALLOC
// hands out objects: xcalloc calls calloc, checks the result and returns
// it. Every return of the wrapper hands back the one allocation in its
// body, so the wrapper's call in main is that allocation; the object is
// named by the allocator call inside the wrapper, and the pair is definite
// in it.

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
  cir.func private @abort()
  cir.func internal private @xcalloc(%n: !u64i, %m: !u64i) -> !cir.ptr<!void> {
    %tmp = cir.alloca "tmp" align(8) : !cir.ptr<!cir.ptr<!void>>
    %r = cir.call @calloc(%n, %m) : (!u64i, !u64i) -> !cir.ptr<!void>
    cir.store %r, %tmp : !cir.ptr<!void>, !cir.ptr<!cir.ptr<!void>>
    cir.scope {
      %t = cir.load %tmp : !cir.ptr<!cir.ptr<!void>>, !cir.ptr<!void>
      %ok = cir.cast ptr_to_bool %t : !cir.ptr<!void> -> !cir.bool
      cir.if %ok {
      } else {
        cir.call @abort() : () -> ()
      }
    }
    %out = cir.load %tmp : !cir.ptr<!cir.ptr<!void>>, !cir.ptr<!void>
    cir.return %out : !cir.ptr<!void>
  }
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

// CHECK: wtc: false sharing in calloc() in @xcalloc: 'a' (offset 0) and 'b' (offset 8)
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
