// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// One thread writes a field, another only reads a neighbour. That still
// bounces the line: the write invalidates the reader's copy, so the reader
// fetches it again every lap. A rule that looked only for two writers would
// miss this, and it is the commoner shape of the two.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @writer() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[0] {name = "counter"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  cir.func @reader() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[1] {name = "config"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.load %m : !cir.ptr<!s64i>, !s64i
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
    %w = cir.get_global @writer : !fnptr
    cir.call @spawn(%w) : (!fnptr) -> ()
    %r = cir.get_global @reader : !fnptr
    cir.call @spawn(%r) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: remark: wtc: false sharing in @s
// CHECK-SAME: 'counter'
// CHECK-SAME: 'config'
