// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The allocation comes through a wrapper that picks between two
// allocators, the way DPDK's test_ring_create picks rte_ring_create or
// rte_ring_create_elem by the element size: each return stores one
// allocation into the front end's `__retval` slot and hands the slot back.
// The wrapper's result is one of the two objects, so the pair is possible
// on each of them and definite on neither; before the sites were listed
// the result was unplaced, "the result of a call that is not an
// allocation", and the program had no objects at all.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @tp = #cir.ptr<null> : !cir.ptr<!rec_S>
  cir.global external @flag = #cir.int<0> : !s32i
  cir.func private @calloc(!u64i, !u64i) -> !cir.ptr<!void>
  cir.func private @malloc(!u64i) -> !cir.ptr<!void>
  cir.func private @realloc(!cir.ptr<!void>, !u64i) -> !cir.ptr<!void>
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func private @abort()
  cir.func internal private @make(%n: !u64i, %flag: !s32i) -> !cir.ptr<!void> {
    %ret = cir.alloca "__retval" align(8) : !cir.ptr<!cir.ptr<!void>>
    cir.scope {
      %c = cir.cast int_to_bool %flag : !s32i -> !cir.bool
      cir.if %c {
        %one = cir.const #cir.int<1> : !u64i
        %r = cir.call @calloc(%one, %n) : (!u64i, !u64i) -> !cir.ptr<!void>
        cir.store %r, %ret : !cir.ptr<!void>, !cir.ptr<!cir.ptr<!void>>
        %o = cir.load %ret : !cir.ptr<!cir.ptr<!void>>, !cir.ptr<!void>
        cir.return %o : !cir.ptr<!void>
      } else {
        %r2 = cir.call @malloc(%n) : (!u64i) -> !cir.ptr<!void>
        cir.store %r2, %ret : !cir.ptr<!void>, !cir.ptr<!cir.ptr<!void>>
        %o2 = cir.load %ret : !cir.ptr<!cir.ptr<!void>>, !cir.ptr<!void>
        cir.return %o2 : !cir.ptr<!void>
      }
    }
    %out = cir.load %ret : !cir.ptr<!cir.ptr<!void>>, !cir.ptr<!void>
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
    %sz = cir.const #cir.int<16> : !u64i
    %fp = cir.get_global @flag : !cir.ptr<!s32i>
    %fv = cir.load %fp : !cir.ptr<!s32i>, !s32i
    %r = cir.call @make(%sz, %fv) : (!u64i, !s32i) -> !cir.ptr<!void>
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

// CHECK-DAG: possible false sharing in calloc() in @make: 'a' and 'b' share one 64-byte line{{.*}}through a pointer that may name one of several objects, this among them
// CHECK-DAG: possible false sharing in malloc() in @make: 'a' and 'b' share one 64-byte line{{.*}}through a pointer that may name one of several objects, this among them
// CHECK: false sharing: 0 definite, 2 possible, 0 cold{{.*}}, 0 field accesses on objects that could not be identified{{.*}}; 2 accesses placed as one of several objects
