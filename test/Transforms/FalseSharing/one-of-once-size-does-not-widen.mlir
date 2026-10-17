// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The once-only variant: thread A writes `s.in.a` once and copies 64
// bytes into `in` through a set, thread B hot-reads `s.b` at offset 64.
// The once-only write of 8 bytes at 0 never meets offset 64, so there is
// no pair, cold or otherwise; the copy's size used to widen it to one.

!s8i = !cir.int<s, 8>
!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_In = !cir.struct<"In" {data !s64i, data !cir.array<!s8i x 56>}>
!rec_S = !cir.struct<"S" {data !rec_In, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @t = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @src = #cir.zero : !rec_In {alignment = 8 : i64}
  cir.global external @flag = #cir.int<0> : !s32i
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa() {
    %slot = cir.alloca "p" align(8) : !cir.ptr<!cir.ptr<!rec_S>>
    %fp = cir.get_global @flag : !cir.ptr<!s32i>
    %fv = cir.load %fp : !cir.ptr<!s32i>, !s32i
    %c = cir.cast int_to_bool %fv : !s32i -> !cir.bool
    cir.if %c {
      %ps = cir.get_global @s : !cir.ptr<!rec_S>
      cir.store %ps, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
      cir.yield
    } else {
      %pt = cir.get_global @t : !cir.ptr<!rec_S>
      cir.store %pt, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
      cir.yield
    }
    %ps = cir.get_global @s : !cir.ptr<!rec_S>
    %in = cir.get_member %ps[0] {name = "in"} : !cir.ptr<!rec_S> -> !cir.ptr<!rec_In>
    %fa = cir.get_member %in[0] {name = "a"} : !cir.ptr<!rec_In> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %fa : !s64i, !cir.ptr<!s64i>
    %p = cir.load %slot : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
    %pin = cir.get_member %p[0] {name = "in"} : !cir.ptr<!rec_S> -> !cir.ptr<!rec_In>
    %src = cir.get_global @src : !cir.ptr<!rec_In>
    cir.copy %src to %pin : !cir.ptr<!rec_In>
    cir.return
  }
  cir.func internal private @wb() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %ps = cir.get_global @s : !cir.ptr<!rec_S>
      %fb = cir.get_member %ps[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %r = cir.load %fb : !cir.ptr<!s64i>, !s64i
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: remark: wtc: false sharing in
// CHECK: false sharing: 0 definite, 0 possible, 0 cold
