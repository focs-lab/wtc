// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 | FileCheck %s

// Two refusals in one file. @table is constant, so no thread writes it and
// it cannot be the write end of a contended pair. @already is aligned to a
// line by whoever wrote the program, and the pass never lowers or restates
// an alignment somebody chose.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global constant external @table = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global external @already = #cir.int<0> : !s64i {alignment = 64 : i64}
  cir.global external @plain = #cir.int<0> : !s64i {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @reads_table() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %t = cir.get_global @table : !cir.ptr<!s64i>
      %r = cir.load %t : !cir.ptr<!s64i>, !s64i
      %p = cir.get_global @already : !cir.ptr<!s64i>
      cir.store %r, %p : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  cir.func @writes_plain() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @plain : !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %p : !s64i, !cir.ptr<!s64i>
      %q = cir.get_global @already : !cir.ptr<!s64i>
      %r = cir.load %q : !cir.ptr<!s64i>, !s64i
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  cir.func @main() {
    %a = cir.get_global @reads_table : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @writes_plain : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: raised the alignment of @table
// CHECK-NOT: raised the alignment of @already
// @table pairs with @plain, not with @already: a different thread writes
// @plain, so @table reaches the candidate set and is refused there.
// CHECK: refused 1 constant, 1 already aligned
// CHECK: @already = #cir.int<0> : !s64i {alignment = 64 : i64}
// CHECK: @plain = #cir.int<0> : !s64i {alignment = 64 : i64}
