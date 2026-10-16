// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A pool of workers, spawned in a loop, writes both fields in loops; main
// writes one of them once, after the spawn. Two workers of the pool may be
// on the two fields, which nobody can tell apart: the answer is possible.
// Main's single store, taken alone, looks cold.
//
// An earlier version answered cold, because cold was tried before possible:
// one extra store by main turned "nothing is known" into "this costs
// nothing". Cold is a resolved claim and now comes last.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
!fnptrA = !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func @spawnA(%fn: !fnptrA, %a: !cir.ptr<!rec_S>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @worker() {
    %n0 = cir.alloca "i0" align(4) : !cir.ptr<!s32i>
    %z0 = cir.const #cir.int<0> : !s32i
    cir.store %z0, %n0 : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n0 : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n0 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n0 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
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
    %n3 = cir.alloca "i3" align(4) : !cir.ptr<!s32i>
    %z3 = cir.const #cir.int<0> : !s32i
    cir.store %z3, %n3 : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n3 : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
    %f2 = cir.get_global @worker : !fnptr
    cir.call @spawn(%f2) : (!fnptr) -> ()
      cir.yield
    } step {
      %c = cir.load %n3 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n3 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
      %pm = cir.get_global @s : !cir.ptr<!rec_S>
      %mm = cir.get_member %pm[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %vm = cir.const #cir.int<1> : !s64i
      cir.store %vm, %mm : !s64i, !cir.ptr<!s64i>
    cir.return
  }
}

// CHECK: possible false sharing in @s
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
