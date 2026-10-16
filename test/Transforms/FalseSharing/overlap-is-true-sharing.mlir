// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Thread A writes in.y in a loop; thread B overwrites the whole of `in` in
// a loop. The two accesses overlap in bytes, so this is true sharing of
// in.y, which no layout removes. Fields were keyed by their start offset,
// and `in` at offset 0 and `in.y` at offset 8 passed as two fields of one
// line: the pass reported them as false sharing, and would have told the
// reader that separating them costs nothing.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_Inner = !cir.struct<"Inner" {data !s64i, data !s64i}>
!rec_Outer = !cir.struct<"Outer" {data !rec_Inner, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @o = #cir.zero : !rec_Outer {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func private @worker_a() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @o : !cir.ptr<!rec_Outer>
      %in = cir.get_member %p[0] {name = "in"} : !cir.ptr<!rec_Outer> -> !cir.ptr<!rec_Inner>
      %y = cir.get_member %in[1] {name = "y"} : !cir.ptr<!rec_Inner> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %y : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func private @worker_b() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @o : !cir.ptr<!rec_Outer>
      %in = cir.get_member %p[0] {name = "in"} : !cir.ptr<!rec_Outer> -> !cir.ptr<!rec_Inner>
      %zero = cir.const #cir.zero : !rec_Inner
      cir.store %zero, %in : !rec_Inner, !cir.ptr<!rec_Inner>
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
    %a = cir.get_global @worker_a : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @worker_b : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: false sharing in
// CHECK: false sharing: 0 definite, 0 possible, 0 cold
