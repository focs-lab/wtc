// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 | FileCheck %s

// As exclusive-arms-raise-nothing.mlir, with a setjmp before the if: a
// longjmp back to it may take the other arm, so the two workers may both
// run, and their globals are not taken to have one owner.

!s64i = !cir.int<s, 64>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func private @_setjmp(!cir.ptr<!s64i>) -> !cir.int<s, 32> attributes {returns_twice}
  cir.global external @env = #cir.int<0> : !s64i
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
  cir.func @main(%c: !cir.bool) {
    %e = cir.get_global @env : !cir.ptr<!s64i>
    %r = cir.call @_setjmp(%e) {returns_twice} : (!cir.ptr<!s64i>) -> !cir.int<s, 32>
    cir.if %c {
      %w = cir.get_global @w1 : !fnptr
      cir.call @spawn(%w) : (!fnptr) -> ()
    } else {
      %x = cir.get_global @w2 : !fnptr
      cir.call @spawn(%x) : (!fnptr) -> ()
    }
    cir.return
  }
}

// CHECK: align by owner: 2 globals raised
