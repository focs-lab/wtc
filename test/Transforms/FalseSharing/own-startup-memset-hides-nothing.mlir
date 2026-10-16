// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// main clears the whole struct with memset before starting any thread, then
// starts a worker that writes b in a loop, and writes a in a loop itself.
// The pair is definite. It vanished once: the widest access main made at
// offset 0 was the 16-byte memset, so main's later 8-byte stores to a were
// taken to overlap b. A repeated access is judged by its own size, not by
// the widest thing its thread ever did at that offset.

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
  cir.func @main() {
    %p0 = cir.get_global @s : !cir.ptr<!rec_S>
    %pv = cir.cast bitcast %p0 : !cir.ptr<!rec_S> -> !cir.ptr<!void>
    %z = cir.const #cir.int<0> : !u8i
    %n = cir.const #cir.int<16> : !u64i
    cir.libc.memset %n bytes at %pv to %z : !cir.ptr<!void>, !u8i, !u64i
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.for : cond {
      %t_n = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t_n)
    } body {
      %p_m = cir.get_global @s : !cir.ptr<!rec_S>
      %f_m = cir.get_member %p_m[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v_m = cir.const #cir.int<1> : !s64i
      cir.store %v_m, %f_m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
}

// CHECK: wtc: false sharing in @s: 'a' (offset 0) and 'b' (offset 8)
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
