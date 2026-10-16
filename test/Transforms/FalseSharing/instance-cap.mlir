// RUN: wtc-opt %s --wtc-false-sharing-diagnose=max-instances=50 2>&1 >/dev/null | FileCheck %s

// Call strings multiply: each level here calls the next from several sites,
// so the number of function instances grows exponentially with depth. Past
// the cap the walk stops adding them, and a verdict computed from a partial
// walk is a verdict about a program that does not exist.
//
// The cap used to be checked by nobody. On memcached it was exceeded
// thirteen-fold and every count in the corpus document came from a walk that
// had silently stopped. The pass now refuses, and says so; the cap is an
// option so that this test does not need a quarter of a million instances.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
!fnptrA = !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func @spawnA(%fn: !fnptrA, %a: !cir.ptr<!rec_S>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @leaf() {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func @f17() {
    cir.call @leaf() : () -> ()
    cir.call @leaf() : () -> ()
    cir.return
  }
  cir.func @f16() {
    cir.call @f17() : () -> ()
    cir.call @f17() : () -> ()
    cir.return
  }
  cir.func @f15() {
    cir.call @f16() : () -> ()
    cir.call @f16() : () -> ()
    cir.return
  }
  cir.func @f14() {
    cir.call @f15() : () -> ()
    cir.call @f15() : () -> ()
    cir.return
  }
  cir.func @f13() {
    cir.call @f14() : () -> ()
    cir.call @f14() : () -> ()
    cir.return
  }
  cir.func @f12() {
    cir.call @f13() : () -> ()
    cir.call @f13() : () -> ()
    cir.return
  }
  cir.func @f11() {
    cir.call @f12() : () -> ()
    cir.call @f12() : () -> ()
    cir.return
  }
  cir.func @f10() {
    cir.call @f11() : () -> ()
    cir.call @f11() : () -> ()
    cir.return
  }
  cir.func @f9() {
    cir.call @f10() : () -> ()
    cir.call @f10() : () -> ()
    cir.return
  }
  cir.func @f8() {
    cir.call @f9() : () -> ()
    cir.call @f9() : () -> ()
    cir.return
  }
  cir.func @f7() {
    cir.call @f8() : () -> ()
    cir.call @f8() : () -> ()
    cir.return
  }
  cir.func @f6() {
    cir.call @f7() : () -> ()
    cir.call @f7() : () -> ()
    cir.return
  }
  cir.func @f5() {
    cir.call @f6() : () -> ()
    cir.call @f6() : () -> ()
    cir.return
  }
  cir.func @f4() {
    cir.call @f5() : () -> ()
    cir.call @f5() : () -> ()
    cir.return
  }
  cir.func @f3() {
    cir.call @f4() : () -> ()
    cir.call @f4() : () -> ()
    cir.return
  }
  cir.func @f2() {
    cir.call @f3() : () -> ()
    cir.call @f3() : () -> ()
    cir.return
  }
  cir.func @f1() {
    cir.call @f2() : () -> ()
    cir.call @f2() : () -> ()
    cir.return
  }
  cir.func @f0() {
    cir.call @f1() : () -> ()
    cir.call @f1() : () -> ()
    cir.return
  }
  cir.func @worker() {
    %n0 = cir.alloca "i0" align(4) : !cir.ptr<!s32i>
    %z0 = cir.const #cir.int<0> : !s32i
    cir.store %z0, %n0 : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n0 : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n0 : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n0 : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %f1 = cir.get_global @worker : !fnptr
    cir.call @spawn(%f1) : (!fnptr) -> ()
    cir.call @f0() : () -> ()
    cir.return
  }
}

// CHECK: gave up after 50 function instances
// CHECK-NOT: false sharing in
// CHECK-NOT: false sharing: 
