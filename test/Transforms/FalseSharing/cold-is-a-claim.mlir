// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Both fields are written in loops, so neither side touches its line once --
// but both threads are placeholders, so nothing is known. The answer is
// unknown, and unknown must not be printed as a resolved one.
//
// It was. The cold test was computed from the unfiltered thread sets and
// returned before the possible branch, so this pair printed "one side
// touches the line once" -- not merely wrong but the opposite of the truth.
// Cold is a claim that the pair costs nothing, so it now needs the same
// accounted-for threads a definite verdict needs.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @edge_a() {
    %na = cir.alloca "ia" align(4) : !cir.ptr<!s32i>
    %za = cir.const #cir.int<0> : !s32i
    cir.store %za, %na : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %na : !cir.ptr<!s32i>, !s32i
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
      %c = cir.load %na : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %na : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @edge_b() {
    %nb = cir.alloca "ib" align(4) : !cir.ptr<!s32i>
    %zb = cir.const #cir.int<0> : !s32i
    cir.store %zb, %nb : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %nb : !cir.ptr<!s32i>, !s32i
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
      %c = cir.load %nb : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %nb : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
}

// CHECK: possible false sharing in @s
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
