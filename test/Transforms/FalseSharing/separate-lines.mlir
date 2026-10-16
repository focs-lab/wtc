// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s
// RUN: sed -e 's/data !pad, //' -e 's/%b\[2\]/%b[1]/' %s | wtc-opt --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s --check-prefix=NOPAD

// The same two threads writing the same struct repeatedly, but the author
// already put 64 bytes between the fields. There is nothing to report, and
// reporting it anyway would be the failure mode that makes a checker
// useless. The second run strips the padding and expects the pair back:
// an earlier version of this test had no loops, so the pair fell out as
// not repeated before the layout was ever looked at, and it passed with
// the padding removed.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!pad = !cir.array<!s64i x 8>
!rec_P = !cir.struct<"P" {data !s64i, data !pad, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @p = #cir.zero : !rec_P {alignment = 64 : i64}

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
      %b = cir.get_global @p : !cir.ptr<!rec_P>
      %m = cir.get_member %b[0] {name = "hits"} : !cir.ptr<!rec_P> -> !cir.ptr<!s64i>
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
      %b = cir.get_global @p : !cir.ptr<!rec_P>
      %m = cir.get_member %b[2] {name = "misses"} : !cir.ptr<!rec_P> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
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

  cir.func @main() {
    %a = cir.get_global @worker_a : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @worker_b : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: remark: wtc: {{(possible )?}}false sharing in
// CHECK: false sharing: 0 definite, 0 possible, 0 cold

// NOPAD: remark: wtc: false sharing in @p: 'hits' (offset 0) and 'misses' (offset 8)
// NOPAD: false sharing: 1 definite, 0 possible, 0 cold
