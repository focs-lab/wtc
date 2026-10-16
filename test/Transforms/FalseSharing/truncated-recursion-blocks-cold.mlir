// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// f(q, r) writes through q and recurses once on r; the worker calls it
// twice, so the second call's recursion is visited and not expanded, and
// its write to s4.a is never seen. A thread with such an unexpanded
// recurrence may be hot-writing any field, so the pair main's start-up
// store makes on s4 with the other worker's hot write is not cold -- it
// used to be, with the hot write unseen.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s1 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @s2 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @s3 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @s4 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @g(%q: !cir.ptr<!rec_S>) {
    %a = cir.get_member %q[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func internal private @f(%q: !cir.ptr<!rec_S>, %r: !cir.ptr<!rec_S>, %c: !cir.bool) {
    cir.call @g(%q) : (!cir.ptr<!rec_S>) -> ()
    cir.if %c {
      %no = cir.const #cir.bool<false> : !cir.bool
      cir.call @f(%r, %r, %no) : (!cir.ptr<!rec_S>, !cir.ptr<!rec_S>, !cir.bool) -> ()
      cir.yield
    }
    cir.return
  }
  cir.func internal private @w1() {
    %p1 = cir.get_global @s1 : !cir.ptr<!rec_S>
    %p2 = cir.get_global @s2 : !cir.ptr<!rec_S>
    %p3 = cir.get_global @s3 : !cir.ptr<!rec_S>
    %p4 = cir.get_global @s4 : !cir.ptr<!rec_S>
    %yes = cir.const #cir.bool<true> : !cir.bool
    cir.call @f(%p1, %p2, %yes) : (!cir.ptr<!rec_S>, !cir.ptr<!rec_S>, !cir.bool) -> ()
    cir.call @f(%p3, %p4, %yes) : (!cir.ptr<!rec_S>, !cir.ptr<!rec_S>, !cir.bool) -> ()
    cir.return
  }
  cir.func internal private @w2() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s4 : !cir.ptr<!rec_S>
      %b = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %b : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %p4 = cir.get_global @s4 : !cir.ptr<!rec_S>
    %a = cir.get_member %p4[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %z = cir.const #cir.int<0> : !s64i
    cir.store %z, %a : !s64i, !cir.ptr<!s64i>
    %w1 = cir.get_global @w1 : !fnptr
    cir.call @spawn(%w1) : (!fnptr) -> ()
    %w2 = cir.get_global @w2 : !fnptr
    cir.call @spawn(%w2) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: wtc: false sharing in
// CHECK: possible false sharing in @s4: 'a' and 'b'{{.*}}accesses it could not place
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
