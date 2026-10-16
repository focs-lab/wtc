// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// main runs init(), which writes a in a loop, then starts the worker that
// writes b, then jumps to its exit label. With a goto in main, where its
// start-up phase ends cannot be read: the jump may go over the spawn or
// back behind it. The analysis used to take the phase as over from the
// first operation and report init's writes as a definite pair with the
// worker's; a guess about start-up is a guess, and the pair is possible.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u8i = !cir.int<u, 8>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wb() {
    cir.for : cond {
      %t_n = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t_n)
    } body {
      %p_wb = cir.get_global @s : !cir.ptr<!rec_S>
      %f_wb = cir.get_member %p_wb[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v_wb = cir.const #cir.int<2> : !s64i
      cir.store %v_wb, %f_wb : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @init() {
    cir.for : cond {
      %t_n = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t_n)
    } body {
      %p_i = cir.get_global @s : !cir.ptr<!rec_S>
      %f_i = cir.get_member %p_i[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v_i = cir.const #cir.int<1> : !s64i
      cir.store %v_i, %f_i : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    cir.call @init() : () -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.goto "out"
  ^bb1:
    cir.label "out"
    cir.return
  }
}

// CHECK-NOT: wtc: false sharing in
// CHECK: possible false sharing in @s: 'a' and 'b'{{.*}}rate the analysis cannot tell
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
