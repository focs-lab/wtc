// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two per-thread counters in one array field, s.cnt[0] and s.cnt[1], each
// bumped by its own thread: the textbook case. The pass once printed
// nothing for it, not even its summary, because an element access was not
// a member access; then it counted the two as accesses it could not place;
// now a constant index is an offset, and the pair is definite.

!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!arr = !cir.array<!s64i x 2>
!rec_S = !cir.struct<"S" {data !arr}>
!fnptr = !cir.ptr<!cir.func<()>>
!void = !cir.void
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %c = cir.get_member %p[0] {name = "cnt"} : !cir.ptr<!rec_S> -> !cir.ptr<!arr>
      %i = cir.const #cir.int<0> : !s64i
      %e = cir.get_element %c[%i : !s64i] : !cir.ptr<!arr> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %e : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @wb() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %c = cir.get_member %p[0] {name = "cnt"} : !cir.ptr<!rec_S> -> !cir.ptr<!arr>
      %i = cir.const #cir.int<1> : !s64i
      %e = cir.get_element %c[%i : !s64i] : !cir.ptr<!arr> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %e : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %w = cir.get_global @wa : !fnptr
    cir.call @spawn(%w) : (!fnptr) -> ()
    %x = cir.get_global @wb : !fnptr
    cir.call @spawn(%x) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in @s: 'cnt' (offset 0) and 'cnt[1]' (offset 8)
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
