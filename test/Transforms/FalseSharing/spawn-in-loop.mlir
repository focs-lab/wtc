// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A worker spawned in a loop stands for many threads, and one instance of it
// touches both fields. The analysis cannot tell one such thread touching two
// fields from two threads touching one each, so it says possible and not
// definite. Claiming definite here would be the same class of error as
// counting spawn sites instead of threads, which this project has already
// paid for once.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @worker() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %m0 = cir.get_member %p[0] {name = "hits"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %m1 = cir.get_member %p[1] {name = "misses"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %m0 : !s64i, !cir.ptr<!s64i>
    cir.store %v, %m1 : !s64i, !cir.ptr<!s64i>
    cir.return
  }

  cir.func @main() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<4> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %w = cir.get_global @worker : !fnptr
      cir.call @spawn(%w) : (!fnptr) -> ()
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
}

// CHECK: possible false sharing in @s
// CHECK: false sharing: 0 definite, 1 possible
