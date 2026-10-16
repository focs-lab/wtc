// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// X, Y and Z call each other. A worker reaches X's write to s.a through Y,
// and main enters the cycle once, during start-up, through a wrapper.
//
// Whether a function reaches a field access was answered by a recursive
// search that cached "no" for a function while it was still being explored.
// Inside a cycle the answer then depended on where the search entered it:
// here main's start-up call entered first, the worker's Y was cached as
// reaching nothing, and the worker's thread never got an instance. The pair
// was reported cold. Reachability is now a fixpoint over the call graph.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
!fnptrA = !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func @spawnA(%fn: !fnptrA, %a: !cir.ptr<!rec_S>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @X() {
    cir.call @Y() : () -> ()
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func @Y() {
    cir.call @Z() : () -> ()
    cir.return
  }
  cir.func @Z() {
    cir.call @X() : () -> ()
    cir.return
  }
  cir.func @wrapper() {
    cir.call @X() : () -> ()
    cir.return
  }
  cir.func @W() {
    %n0 = cir.alloca "i0" align(4) : !cir.ptr<!s32i>
    %z0 = cir.const #cir.int<0> : !s32i
    cir.store %z0, %n0 : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n0 : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      cir.call @Y() : () -> ()
      cir.yield
    } step {
      %c = cir.load %n0 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n0 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @T2() {
    %n1 = cir.alloca "i1" align(4) : !cir.ptr<!s32i>
    %z1 = cir.const #cir.int<0> : !s32i
    cir.store %z1, %n1 : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n1 : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n1 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n1 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    cir.call @wrapper() : () -> ()
    %f2 = cir.get_global @W : !fnptr
    cir.call @spawn(%f2) : (!fnptr) -> ()
    %f3 = cir.get_global @T2 : !fnptr
    cir.call @spawn(%f3) : (!fnptr) -> ()
      %pm = cir.get_global @s : !cir.ptr<!rec_S>
      %mm = cir.get_member %pm[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %vm = cir.const #cir.int<1> : !s64i
      cir.store %vm, %mm : !s64i, !cir.ptr<!s64i>
    cir.return
  }
}

// CHECK: remark: wtc: false sharing in @s
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
