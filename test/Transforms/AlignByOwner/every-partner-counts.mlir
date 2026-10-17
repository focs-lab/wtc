// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 >/dev/null | FileCheck %s

// @x and @y are written by different threads; @table is a constant one of
// them reads. Each global may be paired with several others, and whether
// raising it separates it from anything depends on all of them.
//
// An earlier version remembered only the first partner found, so the
// refusal and the remark were about whichever pair the walk met first.
// Every partner is now kept: @x and @y are raised, each naming the other,
// and @table, which nothing writes, is not.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
!fnptrA = !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>
module {
  cir.global external @x = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global constant external @table = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global external @y = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func @spawnA(%fn: !fnptrA, %a: !cir.ptr<!rec_S>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @writer() {
    %n0 = cir.alloca "i0" align(4) : !cir.ptr<!s32i>
    %z0 = cir.const #cir.int<0> : !s32i
    cir.store %z0, %n0 : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n0 : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @x : !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %p : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n0 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n0 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @reader() {
    %n1 = cir.alloca "i1" align(4) : !cir.ptr<!s32i>
    %z1 = cir.const #cir.int<0> : !s32i
    cir.store %z1, %n1 : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n1 : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %t = cir.get_global @table : !cir.ptr<!s64i>
      %r = cir.load %t : !cir.ptr<!s64i>, !s64i
      cir.yield
    } step {
      %c = cir.load %n1 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n1 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    %n2 = cir.alloca "i2" align(4) : !cir.ptr<!s32i>
    %z2 = cir.const #cir.int<0> : !s32i
    cir.store %z2, %n2 : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n2 : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %t = cir.get_global @y : !cir.ptr<!s64i>
      %r = cir.load %t : !cir.ptr<!s64i>, !s64i
      cir.yield
    } step {
      %c = cir.load %n2 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n2 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %f3 = cir.get_global @writer : !fnptr
    cir.call @spawn(%f3) : (!fnptr) -> ()
    %f4 = cir.get_global @reader : !fnptr
    cir.call @spawn(%f4) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: raised the alignment of @x from 8 to 64 bytes: a different thread repeatedly touches @y
// CHECK: raised the alignment of @y from 8 to 64 bytes: a different thread repeatedly touches @x
// CHECK-NOT: raised the alignment of @table
// CHECK: align by owner: 2 globals raised to 64 bytes
