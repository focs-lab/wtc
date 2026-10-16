// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// main writes a field in a loop AFTER starting a worker that writes its
// neighbour. That is contention and has to be reported.
//
// It was not. The start-up phase was a property of the whole instance, set
// once when the instance was created, and main -- reached without passing a
// spawn, by definition -- had every access in its body treated as start-up,
// before and after the spawn alike. Any program that spawns from main lost
// main's accesses entirely, which is every program using std::thread. The
// phase is now asked per operation.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @worker() {
    %nw = cir.alloca "iw" align(4) : !cir.ptr<!s32i>
    %zw = cir.const #cir.int<0> : !s32i
    cir.store %zw, %nw : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %nw : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[1] {name = "misses"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %nw : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %nw : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  cir.func @main() {
    %w = cir.get_global @worker : !fnptr
    cir.call @spawn(%w) : (!fnptr) -> ()
    %nm = cir.alloca "im" align(4) : !cir.ptr<!s32i>
    %zm = cir.const #cir.int<0> : !s32i
    cir.store %zm, %nm : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %nm : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[0] {name = "hits"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %nm : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %nm : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
}

// CHECK: remark: wtc: false sharing in @s
// CHECK-SAME: 'hits'
// CHECK-SAME: 'misses'
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
