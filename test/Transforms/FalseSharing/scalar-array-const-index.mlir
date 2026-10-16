// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two per-thread counters in one global int array, counts[0] and
// counts[1], each hot-written by its own thread: adjacent, one line,
// definite. An element of an array of scalars has no member name, so the
// report names it by its index. This shape once produced no output at
// all, because an indexed address was not a member access.

!s64i = !cir.int<s, 64>
!arr = !cir.array<!s64i x 16>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @counts = #cir.zero : !arr {alignment = 16 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @w0() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @counts : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %d : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @w1() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @counts : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %p : !cir.ptr<!arr> -> !cir.ptr<!s64i>
      %k = cir.const #cir.int<1> : !s64i
      %e = cir.ptr_stride %d, %k : (!cir.ptr<!s64i>, !s64i) -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %e : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %a = cir.get_global @w0 : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @w1 : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in @counts: '[0]' (offset 0) and '[1]' (offset 8) are touched by different threads and share one 64-byte line
// CHECK: false sharing: 1 definite, 0 possible, 0 cold{{.*}}, 0 field accesses on objects that could not be identified
