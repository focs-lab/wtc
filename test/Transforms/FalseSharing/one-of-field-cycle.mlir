// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A field set from itself: `n->next = m->next`. Listing the stores into
// `next` leads to a load from `next`, whose list is the one being built.
// The walk answers nothing for the load rather than looping, and the
// access through it stays unplaced.

!s64i = !cir.int<s, 64>
!rec_N = !cir.struct<"N" {data !s64i, data !cir.ptr<!cir.struct<"N">>}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @n = #cir.zero : !rec_N {alignment = 8 : i64}
  cir.global external @m = #cir.zero : !rec_N {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @link() {
    %mp = cir.get_global @m : !cir.ptr<!rec_N>
    %mn = cir.get_member %mp[1] {name = "next"} : !cir.ptr<!rec_N> -> !cir.ptr<!cir.ptr<!rec_N>>
    %q = cir.load %mn : !cir.ptr<!cir.ptr<!rec_N>>, !cir.ptr<!rec_N>
    %np = cir.get_global @n : !cir.ptr<!rec_N>
    %nn = cir.get_member %np[1] {name = "next"} : !cir.ptr<!rec_N> -> !cir.ptr<!cir.ptr<!rec_N>>
    cir.store %q, %nn : !cir.ptr<!rec_N>, !cir.ptr<!cir.ptr<!rec_N>>
    cir.return
  }
  cir.func internal private @wa() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %np = cir.get_global @n : !cir.ptr<!rec_N>
      %nn = cir.get_member %np[1] {name = "next"} : !cir.ptr<!rec_N> -> !cir.ptr<!cir.ptr<!rec_N>>
      %q = cir.load %nn : !cir.ptr<!cir.ptr<!rec_N>>, !cir.ptr<!rec_N>
      %f = cir.get_member %q[0] {name = "v"} : !cir.ptr<!rec_N> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    cir.call @link() : () -> ()
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: false sharing: 0 definite, 0 possible, 0 cold{{.*}}, 1 field accesses on objects that could not be identified{{.*}}; of the unidentified, 1 through a pointer loaded from a field{{.*}}; 0 accesses placed as one of several objects
