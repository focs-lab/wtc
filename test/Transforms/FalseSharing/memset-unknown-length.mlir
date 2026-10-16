// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A memset whose length is loaded, not a constant. The access is placed
// with the size of what the program's own pointer points at -- the member
// behind the void* cast -- rather than asked of the void type, which the
// data layout cannot size: the pass used to abort here.

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
  cir.global external @len = #cir.int<8> : !u64i {alignment = 8 : i64}
  cir.func internal private @wa() {
    cir.for : cond {
      %t_n = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t_n)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %av = cir.cast bitcast %a : !cir.ptr<!s64i> -> !cir.ptr<!void>
      %z = cir.const #cir.int<0> : !u8i
      %lp = cir.get_global @len : !cir.ptr<!u64i>
      %n = cir.load %lp : !cir.ptr<!u64i>, !u64i
      cir.libc.memset %n bytes at %av to %z : !cir.ptr<!void>, !u8i, !u64i
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in @s: 'a' (offset 0) and 'b' (offset 8)
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
