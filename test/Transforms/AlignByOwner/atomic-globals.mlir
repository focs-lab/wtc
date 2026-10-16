// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 | FileCheck %s

// Two scalar globals, each bumped by its own thread with an atomic
// fetch-add: the pair the pass exists for, written the way lock-free code
// writes it. The pass saw loads and stores only and printed nothing here,
// not even its summary.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @hits = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global external @misses = #cir.int<0> : !s64i {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func private @hit() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @hits : !cir.ptr<!s64i>
      %one = cir.const #cir.int<1> : !s64i
      %old = cir.atomic.fetch add relaxed syncscope(system) fetch_first %p, %one : (!cir.ptr<!s64i>, !s64i) -> !s64i
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func private @miss() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @misses : !cir.ptr<!s64i>
      %one = cir.const #cir.int<1> : !s64i
      %old = cir.atomic.fetch add relaxed syncscope(system) fetch_first %p, %one : (!cir.ptr<!s64i>, !s64i) -> !s64i
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
    %a = cir.get_global @hit : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @miss : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-DAG: raised the alignment of @hits from 8 to 64
// CHECK-DAG: raised the alignment of @misses from 8 to 64
// CHECK: align by owner: 2 globals raised to 64 bytes
// CHECK: @hits = #cir.int<0> : !s64i {alignment = 64 : i64}
// CHECK: @misses = #cir.int<0> : !s64i {alignment = 64 : i64}
