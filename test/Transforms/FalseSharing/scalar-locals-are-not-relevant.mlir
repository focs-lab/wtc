// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// main calls a function that stores only to a local scalar, twice, one
// that stores only to a scalar global, and one that touches nothing, and
// then writes s.a in a loop. None of the three can take part in a pair of
// fields, so none needs an instance; a version that treated any access to
// a global or a slot as relevant gave every one of them an instance, which
// on real output brings the standard library's call graph into the walk.

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
  cir.global external @g = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.func internal private @local_only() {
    %l = cir.alloca "x" align(8) : !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %l : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func internal private @scalar_global_only() {
    %g = cir.get_global @g : !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %g : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func internal private @nothing() { cir.return }
  cir.func @main() {
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.call @local_only() : () -> ()
    cir.call @local_only() : () -> ()
    cir.call @scalar_global_only() : () -> ()
    cir.call @nothing() : () -> ()
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
// CHECK: false sharing: 1 definite, 0 possible, 0 cold{{.*}}; 2 function instances
