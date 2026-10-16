// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 | FileCheck %s

// An alias has no storage of its own: raising its alignment moves nothing
// and says "raised" about an object that did not move. It is left alone.

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
  cir.global external @real = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global external @g1 alias(@real) : !s64i
  cir.global external @g2 = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.func @main() {
    %w = cir.get_global @w1 : !fnptr
    cir.call @spawn(%w) : (!fnptr) -> ()
    %x = cir.get_global @w2 : !fnptr
    cir.call @spawn(%x) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: left @g1 where it is: it is an alias
// CHECK-NOT: raised the alignment of @g1
