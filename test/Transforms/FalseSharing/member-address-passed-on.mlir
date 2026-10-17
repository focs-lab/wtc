// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The address of a member is handed to a routine that works on it, the
// way a ring's producer and consumer heads go to the one routine that
// moves a head: `move(&q.prod)` from one thread, `move(&q.cons)` from the
// other, and `d->h = ...` inside. The access in the routine is on the
// member's record, so many bytes in; the two heads are 16 bytes apart on
// one line, a definite pair. Before the walk stepped through a member's
// address, the access was unplaced, "otherwise", and the pair unseen.

!s64i = !cir.int<s, 64>
!void = !cir.void
!rec_HT = !cir.struct<"HT" {data !s64i, data !s64i}>
!rec_Q = !cir.struct<"Q" {data !s64i, data !cir.struct<"HT" {data !s64i, data !s64i}>, data !cir.struct<"HT" {data !s64i, data !s64i}>}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @q = #cir.zero : !rec_Q {alignment = 64 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @move(%d: !cir.ptr<!rec_HT>) {
    %slot = cir.alloca "d" align(8) : !cir.ptr<!cir.ptr<!rec_HT>>
    cir.store %d, %slot : !cir.ptr<!rec_HT>, !cir.ptr<!cir.ptr<!rec_HT>>
    %p = cir.load %slot : !cir.ptr<!cir.ptr<!rec_HT>>, !cir.ptr<!rec_HT>
    %h = cir.get_member %p[0] {name = "h"} : !cir.ptr<!rec_HT> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %h : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func internal private @producer() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %qp = cir.get_global @q : !cir.ptr<!rec_Q>
      %prod = cir.get_member %qp[1] {name = "prod"} : !cir.ptr<!rec_Q> -> !cir.ptr<!rec_HT>
      cir.call @move(%prod) : (!cir.ptr<!rec_HT>) -> ()
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @consumer() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %qp = cir.get_global @q : !cir.ptr<!rec_Q>
      %cons = cir.get_member %qp[2] {name = "cons"} : !cir.ptr<!rec_Q> -> !cir.ptr<!rec_HT>
      cir.call @move(%cons) : (!cir.ptr<!rec_HT>) -> ()
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %a = cir.get_global @producer : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @consumer : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in @q: 'h' (offset 8) and 'h' (offset 24) are touched by different threads and share one 64-byte line
// CHECK: false sharing: 1 definite, 0 possible, 0 cold{{.*}}, 0 field accesses on objects that could not be identified
