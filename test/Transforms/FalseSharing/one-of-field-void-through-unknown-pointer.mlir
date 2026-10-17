// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The guard: the function pointer handed to `run` is loaded from a
// global, so the call's targets are not known, and the record's address
// as `void *` reaching it marks the record written whole: the list says
// nothing and the load stays unplaced.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
!rec_C = !cir.struct<"C" {data !cir.ptr<!cir.struct<"S" {data !s64i, data !s64i}>>}>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @t = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @flag = #cir.int<0> : !s32i
  cir.global external @hook = #cir.ptr<null> : !cir.ptr<!cir.func<(!cir.ptr<!void>)>>
  cir.global external @c = #cir.zero : !rec_C {alignment = 8 : i64}
  cir.func internal private @run(%fn: !cir.ptr<!cir.func<(!cir.ptr<!void>)>>, %p: !cir.ptr<!void>) {
    cir.call %fn(%p) : (!cir.ptr<!cir.func<(!cir.ptr<!void>)>>, !cir.ptr<!void>) -> ()
    cir.return
  }
  cir.func internal private @fill(%p: !cir.ptr<!void>) {
    %c = cir.cast bitcast %p : !cir.ptr<!void> -> !cir.ptr<!rec_C>
    %tf = cir.get_member %c[0] {name = "t"} : !cir.ptr<!rec_C> -> !cir.ptr<!cir.ptr<!rec_S>>
    %t = cir.load %tf : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
    %a = cir.get_member %t[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.load %a : !cir.ptr<!s64i>, !s64i
    cir.return
  }
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @bind_s() {
    %cp = cir.get_global @c : !cir.ptr<!rec_C>
    %tf = cir.get_member %cp[0] {name = "t"} : !cir.ptr<!rec_C> -> !cir.ptr<!cir.ptr<!rec_S>>
    %x = cir.get_global @s : !cir.ptr<!rec_S>
    cir.store %x, %tf : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    cir.return
  }
  cir.func internal private @bind_t() {
    %cp = cir.get_global @c : !cir.ptr<!rec_C>
    %tf = cir.get_member %cp[0] {name = "t"} : !cir.ptr<!rec_C> -> !cir.ptr<!cir.ptr<!rec_S>>
    %x = cir.get_global @t : !cir.ptr<!rec_S>
    cir.store %x, %tf : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    cir.return
  }
  cir.func internal private @wa() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %cp = cir.get_global @c : !cir.ptr<!rec_C>
      %tf = cir.get_member %cp[0] {name = "t"} : !cir.ptr<!rec_C> -> !cir.ptr<!cir.ptr<!rec_S>>
      %p = cir.load %tf : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
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
      %ps = cir.get_global @s : !cir.ptr<!rec_S>
      %fs = cir.get_member %ps[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %vs = cir.const #cir.int<2> : !s64i
      cir.store %vs, %fs : !s64i, !cir.ptr<!s64i>
      %pt = cir.get_global @t : !cir.ptr<!rec_S>
      %ft = cir.get_member %pt[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %vt = cir.const #cir.int<3> : !s64i
      cir.store %vt, %ft : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %fp = cir.get_global @flag : !cir.ptr<!s32i>
    %fv = cir.load %fp : !cir.ptr<!s32i>, !s32i
    %c = cir.cast int_to_bool %fv : !s32i -> !cir.bool
    cir.if %c {
      cir.call @bind_s() : () -> ()
      cir.yield
    } else {
      cir.call @bind_t() : () -> ()
      cir.yield
    }
    %cp2 = cir.get_global @c : !cir.ptr<!rec_C>
    %cv2 = cir.cast bitcast %cp2 : !cir.ptr<!rec_C> -> !cir.ptr<!void>
    %hp = cir.get_global @hook : !cir.ptr<!cir.ptr<!cir.func<(!cir.ptr<!void>)>>>
    %fnp = cir.load %hp : !cir.ptr<!cir.ptr<!cir.func<(!cir.ptr<!void>)>>>, !cir.ptr<!cir.func<(!cir.ptr<!void>)>>
    cir.call @run(%fnp, %cv2) : (!cir.ptr<!cir.func<(!cir.ptr<!void>)>>, !cir.ptr<!void>) -> ()
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: false sharing in
// CHECK: false sharing: 0 definite, 0 possible, 0 cold{{.*}}, 1 field accesses on objects that could not be identified{{.*}}; of the unidentified, 1 through a pointer loaded from a field{{.*}}; 0 accesses placed as one of several objects
