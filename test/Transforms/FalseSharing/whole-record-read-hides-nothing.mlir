// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// main reads the whole nested record once at start-up; then one thread
// writes in.x in a loop and another writes in.y. The pair is definite. It
// vanished: the field at offset 0 took the size of the widest access anyone
// made there -- the 16-byte snapshot -- and the overlap rule then called
// in.x and in.y the same data. Overlap is judged on the two accesses that
// make the verdict, not on the widest access at the offset.

!s64i = !cir.int<s, 64>
!rec_I = !cir.struct<"I" {data !s64i, data !s64i}>
!rec_O = !cir.struct<"O" {data !rec_I, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @o = #cir.zero : !rec_O {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wx() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @o : !cir.ptr<!rec_O>
      %i = cir.get_member %p[0] {name = "in"} : !cir.ptr<!rec_O> -> !cir.ptr<!rec_I>
      %x = cir.get_member %i[0] {name = "x"} : !cir.ptr<!rec_I> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %x : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @wy() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @o : !cir.ptr<!rec_O>
      %i = cir.get_member %p[0] {name = "in"} : !cir.ptr<!rec_O> -> !cir.ptr<!rec_I>
      %y = cir.get_member %i[1] {name = "y"} : !cir.ptr<!rec_I> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %y : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    // one start-up read of the whole inner record: `struct I snap = o.in;`
    %p = cir.get_global @o : !cir.ptr<!rec_O>
    %i = cir.get_member %p[0] {name = "in"} : !cir.ptr<!rec_O> -> !cir.ptr<!rec_I>
    %snap = cir.load %i : !cir.ptr<!rec_I>, !rec_I
    %w = cir.get_global @wx : !fnptr
    cir.call @spawn(%w) : (!fnptr) -> ()
    %x = cir.get_global @wy : !fnptr
    cir.call @spawn(%x) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in @o: 'in.x' (offset 0) and 'in.y' (offset 8)
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
