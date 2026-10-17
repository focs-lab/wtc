// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A pointer local assigned nine times, each to a different object: a set
// that large says nothing useful about where the pointer points, and
// carrying it would pair every one of its objects. Past eight sources
// the load stays unplaced, and the summary says that is why.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @g0 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @g1 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @g2 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @g3 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @g4 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @g5 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @g6 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @g7 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @g8 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa() {
    %slot = cir.alloca "p" align(8) : !cir.ptr<!cir.ptr<!rec_S>>
    %p0 = cir.get_global @g0 : !cir.ptr<!rec_S>
    cir.store %p0, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %p1 = cir.get_global @g1 : !cir.ptr<!rec_S>
    cir.store %p1, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %p2 = cir.get_global @g2 : !cir.ptr<!rec_S>
    cir.store %p2, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %p3 = cir.get_global @g3 : !cir.ptr<!rec_S>
    cir.store %p3, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %p4 = cir.get_global @g4 : !cir.ptr<!rec_S>
    cir.store %p4, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %p5 = cir.get_global @g5 : !cir.ptr<!rec_S>
    cir.store %p5, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %p6 = cir.get_global @g6 : !cir.ptr<!rec_S>
    cir.store %p6, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %p7 = cir.get_global @g7 : !cir.ptr<!rec_S>
    cir.store %p7, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %p8 = cir.get_global @g8 : !cir.ptr<!rec_S>
    cir.store %p8, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
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
  cir.func @main() {
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: false sharing: 0 definite, 0 possible, 0 cold{{.*}}, 1 field accesses on objects that could not be identified{{.*}}, 1 from a pointer with more than 8 sources or sources in more than 64 instances, 0 otherwise; 0 accesses placed as one of several objects
