// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 | FileCheck %s

// An alias has no storage of its own. The accesses through @g1 are
// accesses to @real, so it is @real that pairs with @g2 and @real that is
// raised; the alias keeps no alignment of its own. An earlier version made
// the alias a root, raised it, and said "raised" about storage that had
// not moved.

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

// CHECK-DAG: raised the alignment of @real from 8 to 64
// CHECK-DAG: raised the alignment of @g2 from 8 to 64
// CHECK: align by owner: 2 globals raised
// CHECK: @real = #cir.int<0> : !s64i {alignment = 64 : i64}
// CHECK: @g1 alias(@real) : !s64i{{$}}
