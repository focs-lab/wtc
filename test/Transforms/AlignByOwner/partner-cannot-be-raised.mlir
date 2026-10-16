// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 | FileCheck %s

// @table is constant, so it is never raised. Raising @plain alone buys
// nothing: an 8-byte object on its own 64-byte line leaves 56 bytes of tail
// for @table to land in, and the guarantee the remark asserts is not
// achieved.
//
// It was raised anyway, and the remark said "a different thread repeatedly
// touches @table" as though the two had been separated. The comment in the
// pass stated the invariant correctly and the code below it broke it.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global constant external @table = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global external @plain = #cir.int<0> : !s64i {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @reader() {
    %nr = cir.alloca "ir" align(4) : !cir.ptr<!s32i>
    %zr = cir.const #cir.int<0> : !s32i
    cir.store %zr, %nr : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %nr : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %t = cir.get_global @table : !cir.ptr<!s64i>
      %r = cir.load %t : !cir.ptr<!s64i>, !s64i
      cir.yield
    } step {
      %c = cir.load %nr : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %nr : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @writer() {
    %nw = cir.alloca "iw" align(4) : !cir.ptr<!s32i>
    %zw = cir.const #cir.int<0> : !s32i
    cir.store %zw, %nw : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %nw : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @plain : !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %p : !s64i, !cir.ptr<!s64i>
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
    %a = cir.get_global @reader : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @writer : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: raised the alignment
// CHECK: align by owner: 0 globals raised to 64 bytes, at most 0 bytes of padding; refused 1 constant, 0 already aligned, 1 whose partner could not be raised
// CHECK-NOT: alignment = 64
