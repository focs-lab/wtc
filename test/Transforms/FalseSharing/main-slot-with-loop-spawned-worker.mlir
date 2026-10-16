// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A struct in main, its address handed to a worker spawned in a loop and
// to one spawned once. main runs once, so its slot is one object, and the
// pair is definite. The slot was taken for many because the *accessing*
// instance stood for many threads; whether a slot is one object is a
// property of the function that owns it.


// A struct declared in main, its address handed to two threads, each writing
// one field. This is the textbook shape of the bug, and it was unreachable.
//
// The rule "a stack slot is a different object in every invocation" is true
// and was applied unconditionally, so every stack slot was downgraded to
// possible. It does not hold for a function that runs exactly once, and main
// is the case that matters. The instance already carried the evidence --
// execMult and threadMult -- and nothing consulted it.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>
module {
  cir.func @spawn(%fn: !fnptr, %a: !cir.ptr<!rec_S>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func internal private @worker_a(%arg0: !cir.ptr<!rec_S>) {
    %na = cir.alloca "ia" align(4) : !cir.ptr<!s32i>
    %za = cir.const #cir.int<0> : !s32i
    cir.store %za, %na : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %na : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %m = cir.get_member %arg0[0] {name = "hits"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %na : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %na : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func internal private @worker_b(%arg0: !cir.ptr<!rec_S>) {
    %nb = cir.alloca "ib" align(4) : !cir.ptr<!s32i>
    %zb = cir.const #cir.int<0> : !s32i
    cir.store %zb, %nb : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %nb : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %m = cir.get_member %arg0[1] {name = "misses"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %nb : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %nb : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  cir.func @main() {
    %s = cir.alloca "s" align(8) : !cir.ptr<!rec_S>
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %a = cir.get_global @worker_a : !fnptr
      cir.call @spawn(%a, %s) : (!fnptr, !cir.ptr<!rec_S>) -> ()
      cir.yield
    } step {
      cir.yield
    }
    %b = cir.get_global @worker_b : !fnptr
    cir.call @spawn(%b, %s) : (!fnptr, !cir.ptr<!rec_S>) -> ()
    cir.return
  }
}


// CHECK: wtc: false sharing in %s
// CHECK-SAME: 'hits'
// CHECK-SAME: 'misses'
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
