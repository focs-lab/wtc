// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The checked wrapper returns NULL when the allocation fails and the
// allocation otherwise. A null hands back no object a field access could
// reach, so the wrapper still hands back one allocation; the null return
// used to make the wrapper no allocation at all.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.func private @calloc(!u64i, !u64i) -> !cir.ptr<!void>
  cir.func @spawn(%fn: !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>, %arg: !cir.ptr<!rec_S>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @xcalloc(%n: !u64i, %m: !u64i) -> !cir.ptr<!void> {
    %tmp = cir.alloca "tmp" align(8) : !cir.ptr<!cir.ptr<!void>>
    %r = cir.call @calloc(%n, %m) : (!u64i, !u64i) -> !cir.ptr<!void>
    cir.store %r, %tmp : !cir.ptr<!void>, !cir.ptr<!cir.ptr<!void>>
    cir.scope {
      %t = cir.load %tmp : !cir.ptr<!cir.ptr<!void>>, !cir.ptr<!void>
      %ok = cir.cast ptr_to_bool %t : !cir.ptr<!void> -> !cir.bool
      cir.if %ok {
      } else {
        %null = cir.const #cir.ptr<null> : !cir.ptr<!void>
        cir.return %null : !cir.ptr<!void>
      }
    }
    %out = cir.load %tmp : !cir.ptr<!cir.ptr<!void>>, !cir.ptr<!void>
    cir.return %out : !cir.ptr<!void>
  }
  cir.func internal private @pa(%p: !cir.ptr<!rec_S>) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %f = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @pb(%p: !cir.ptr<!rec_S>) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
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
    %a = cir.get_global @pa : !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>
    cir.call @spawn(%a, %rs) : (!cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>, !cir.ptr<!rec_S>) -> ()
    %b = cir.get_global @pb : !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>
    cir.call @spawn(%b, %rs) : (!cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>, !cir.ptr<!rec_S>) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in calloc() in @xcalloc: 'a' (offset 0) and 'b' (offset 8) are touched by different threads
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
