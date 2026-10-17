// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A pointer local assigned on both arms of an `if`, `p = &s` or `p = &t`,
// then written through in a loop: the walk places the write on both
// objects, as one of the two. Against a thread that hot-writes the
// neighbouring field of each, that is a possible pair on each object and
// never a definite one: the pair exists only if the pointer names this
// object. The write used to be unplaced.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @t = #cir.zero : !rec_S {alignment = 8 : i64}
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
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.load %slot : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
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
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: remark: wtc: false sharing in
// CHECK-DAG: possible false sharing in @s: 'a' and 'b' share one 64-byte line in 7 of the 8 positions an object aligned to 8 bytes may start at and are touched repeatedly, but through a pointer that may name one of several objects, this among them
// CHECK-DAG: possible false sharing in @t: 'a' and 'b' share one 64-byte line in 7 of the 8 positions an object aligned to 8 bytes may start at and are touched repeatedly, but through a pointer that may name one of several objects, this among them
// CHECK: false sharing: 0 definite, 2 possible, 0 cold{{.*}}, 0 field accesses on objects that could not be identified{{.*}}; 1 accesses placed as one of several objects
