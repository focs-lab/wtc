// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A global constructor starts a worker before main runs. Main's writes are
// therefore not start-up: a thread already exists to contend with them.
//
// The walk used to give main the start-up flag unconditionally, so main's
// writes were taken for start-up and the pair came out cold.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
!fnptrA = !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>
module attributes {cir.global_ctors = [#cir.global_ctor<"init", 65535>]} {
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
      %m = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n0 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n0 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func internal private @init() {
    %f1 = cir.get_global @worker : !fnptr
    cir.call @spawn(%f1) : (!fnptr) -> ()
    cir.return
  }
  cir.func @main() {
    %n2 = cir.alloca "i2" align(4) : !cir.ptr<!s32i>
    %z2 = cir.const #cir.int<0> : !s32i
    cir.store %z2, %n2 : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n2 : !cir.ptr<!s32i>, !s32i
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
      %c = cir.load %n2 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n2 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
}

// CHECK: remark: wtc: false sharing in @s
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
