// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 >/dev/null | FileCheck %s

// main fills two globals in a loop and only then starts a worker. The
// writes are over before the worker exists, so there is no line for two
// threads to pass between them.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @cfg_a = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global external @cfg_b = #cir.int<0> : !s64i {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @worker() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @cfg_b : !cir.ptr<!s64i>
      %r = cir.load %p : !cir.ptr<!s64i>, !s64i
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
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @cfg_a : !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %p : !s64i, !cir.ptr<!s64i>
      %q = cir.get_global @cfg_b : !cir.ptr<!s64i>
      cir.store %v, %q : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    %w = cir.get_global @worker : !fnptr
    cir.call @spawn(%w) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: raised the alignment
// CHECK: align by owner: 0 globals raised
