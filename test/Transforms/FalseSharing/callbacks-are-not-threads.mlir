// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two functions whose addresses are stored in a table -- which is what a
// callback registration looks like once the names are gone. They may be two
// threads. They may equally be two handlers of one single-threaded event
// loop, or two comparators handed to qsort, and nothing here says which.
//
// They were being given two distinct accounted-for thread identifiers on the
// strength of "its address is taken here, so it plausibly runs as a thread".
// A definite verdict does not claim plausibly. Both are now speculative:
// they may reach possible, never definite.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @on_read = #cir.global_view<@handler_a> : !fnptr
  cir.global external @on_write = #cir.global_view<@handler_b> : !fnptr

  cir.func @handler_a() {
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

  cir.func @handler_b() {
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

  cir.func @main() { cir.return }
}

// CHECK-NOT: wtc: false sharing in @s
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
