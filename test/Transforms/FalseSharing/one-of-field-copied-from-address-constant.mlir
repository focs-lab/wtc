// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The record holding the pointer field is copied whole out of a constant
// that names an object, `c = { &u }`: the copy writes the field with an
// address no store lists, so the list says nothing and the load stays
// unplaced. The guard for the copy-from-a-constant rule, which admits only
// constants without an address in them.

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
  cir.global external @c = #cir.zero : !rec_C {alignment = 8 : i64}
  cir.global external @u = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global constant external @init = #cir.const_record<{#cir.global_view<@u> : !cir.ptr<!rec_S>}> : !rec_C {alignment = 8 : i64}
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
    %src = cir.get_global @init : !cir.ptr<!rec_C>
    %dst = cir.get_global @c : !cir.ptr<!rec_C>
    cir.copy %src to %dst : !cir.ptr<!rec_C>
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: possible false sharing
// CHECK: false sharing: 0 definite, 0 possible, 0 cold{{.*}}, 1 field accesses on objects that could not be identified{{.*}}; of the unidentified, 1 through a pointer loaded from a field{{.*}}; 0 accesses placed as one of several objects
