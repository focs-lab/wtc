// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 | FileCheck %s

// main's start-up store, in a main that also has a goto: the rate is a
// guess and nothing is raised.

!s64i = !cir.int<s, 64>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @w1() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @g1 : !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %p : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @w2() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @g2 : !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %p : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.global external @g1 = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global external @g2 = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.func @main() {
    %p = cir.get_global @g1 : !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %p : !s64i, !cir.ptr<!s64i>
    cir.goto "go"
  ^bb1:
    cir.label "go"
    %x = cir.get_global @w2 : !fnptr
    cir.call @spawn(%x) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: raised the alignment
// CHECK: align by owner: 0 globals raised
// CHECK-NOT: alignment = 64
