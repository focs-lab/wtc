// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// run() declares a local struct and starts the worker it is given on it.
// main calls run twice, with a worker that writes field a and then one
// that writes field b. Two calls, two slots: the two workers write two
// objects, and the second cannot share a line with the first. The slot was
// keyed by its alloca alone, so the two instances of run merged into one
// object and the pair came out definite. A slot of a function the walk
// instantiated more than once is not one object, and can reach possible
// at most.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!voidptr = !cir.ptr<!cir.void>
!fnptr = !cir.ptr<!cir.func<(!voidptr)>>
module {
  cir.func @spawn(%fn: !fnptr, %arg: !voidptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func @join() { cir.return }

  cir.func private @worker_a(%arg: !voidptr) {
    %p = cir.cast bitcast %arg : !voidptr -> !cir.ptr<!rec_S>
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %a : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func private @worker_b(%arg: !voidptr) {
    %p = cir.cast bitcast %arg : !voidptr -> !cir.ptr<!rec_S>
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %b = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
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

  cir.func private @run(%fn: !fnptr) {
    %local = cir.alloca "local" align(8) : !cir.ptr<!rec_S>
    %arg = cir.cast bitcast %local : !cir.ptr<!rec_S> -> !voidptr
    cir.call @spawn(%fn, %arg) : (!fnptr, !voidptr) -> ()
    cir.call @join() : () -> ()
    cir.return
  }

  cir.func @main() {
    %a = cir.get_global @worker_a : !fnptr
    cir.call @run(%a) : (!fnptr) -> ()
    %b = cir.get_global @worker_b : !fnptr
    cir.call @run(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: wtc: false sharing in
// CHECK: possible false sharing in %local: 'a' and 'b'{{.*}}stack slot whose address escapes
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
