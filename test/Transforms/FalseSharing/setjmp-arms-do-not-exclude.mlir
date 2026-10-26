// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// As spawn-in-either-branch.mlir, with a setjmp before the if: a longjmp
// back to it runs the if again and may take the other arm, so the two
// spawns are not exclusive, and the pair is counted as for any two spawns
// the analysis cannot tell apart.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func private @_setjmp(!cir.ptr<!s64i>) -> !cir.int<s, 32> attributes {returns_twice}
  cir.global external @env = #cir.int<0> : !s64i

  cir.func private @worker() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %b = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %a : !s64i, !cir.ptr<!s64i>
      cir.store %v, %b : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  cir.func @main(%c: !cir.bool) {
    %e = cir.get_global @env : !cir.ptr<!s64i>
    %r = cir.call @_setjmp(%e) {returns_twice} : (!cir.ptr<!s64i>) -> !cir.int<s, 32>
    cir.if %c {
      %w = cir.get_global @worker : !fnptr
      cir.call @spawn(%w) : (!fnptr) -> ()
      cir.yield
    } else {
      %w = cir.get_global @worker : !fnptr
      cir.call @spawn(%w) : (!fnptr) -> ()
      cir.yield
    }
    cir.return
  }
}

// CHECK: wtc: false sharing in @s: 'a' (offset 0) and 'b' (offset 8) are touched by different threads
// CHECK: false sharing: 1 definite, 0 possible
