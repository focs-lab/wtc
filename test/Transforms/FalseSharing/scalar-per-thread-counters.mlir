// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The textbook per-thread counter: a global array of longs, each thread
// handed &counts[i] by the spawn loop and bumping *me in a loop. Each
// element is 8 bytes, so neighbouring threads' counters share a line
// wherever the array starts but at a line boundary: definite across
// elements. The element is handed out with cir.get_element, which the
// walk now reads like a pointer stride; and a scalar through a parameter
// is an access, which it was not.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!arr = !cir.array<!s64i x 8>
!fnptr = !cir.ptr<!cir.func<(!cir.ptr<!s64i>)>>
module {
  cir.global external @counts = #cir.zero : !arr {alignment = 16 : i64}
  cir.func @spawn(%fn: !fnptr, %arg: !cir.ptr<!s64i>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @worker(%me: !cir.ptr<!s64i>) {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %me : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %i = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %i : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<8> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @counts : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!s64i>
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %k = cir.cast integral %c : !s32i -> !s64i
      %elem = cir.ptr_stride %d, %k : (!cir.ptr<!s64i>, !s64i) -> !cir.ptr<!s64i>
      %f = cir.get_global @worker : !fnptr
      cir.call @spawn(%f, %elem) : (!fnptr, !cir.ptr<!s64i>) -> ()
      cir.yield
    } step {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %inc = cir.inc nsw %c : !s32i
      cir.store %inc, %i : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
}

// CHECK: wtc: false sharing in @counts (one element per thread) across elements: '<whole>' (offset 0) of one element and '<whole>' (offset 0) of the next are touched by the two threads the spawn loop handed them to and share one 64-byte line in 7 of the 8 positions an element of 8 bytes may start at
// CHECK: false sharing: 1 definite, 0 possible, 0 cold{{.*}}, 0 field accesses on objects that could not be identified
