// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// f writes its argument's field a through a helper and, when told, calls
// itself on s2. main calls f on s3 during start-up; a worker calls f on s
// with the recursive call enabled; another worker writes s2.b in a loop.
// The walk unrolls a recurrence once -- and once per function alone, the
// start-up call took the unrolling, so the worker's recursive call, the
// one that reaches s2, was only visited and its write never seen: the pair
// came out cold. Once per function, thread and phase.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wb() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %f = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.global external @s2 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @s3 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func internal private @g(%q: !cir.ptr<!rec_S>) {
    %a = cir.get_member %q[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func internal private @f(%q: !cir.ptr<!rec_S>, %c: !cir.bool) {
    cir.call @g(%q) : (!cir.ptr<!rec_S>) -> ()
    cir.if %c {
      %o = cir.get_global @s2 : !cir.ptr<!rec_S>
      %no = cir.const #cir.bool<false> : !cir.bool
      cir.call @f(%o, %no) : (!cir.ptr<!rec_S>, !cir.bool) -> ()
      cir.yield
    }
    cir.return
  }
  cir.func internal private @w1() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %yes = cir.const #cir.bool<true> : !cir.bool
    cir.call @f(%p, %yes) : (!cir.ptr<!rec_S>, !cir.bool) -> ()
    cir.return
  }
  cir.func internal private @w2() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s2 : !cir.ptr<!rec_S>
      %f = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %p3 = cir.get_global @s3 : !cir.ptr<!rec_S>
    %no = cir.const #cir.bool<false> : !cir.bool
    cir.call @f(%p3, %no) : (!cir.ptr<!rec_S>, !cir.bool) -> ()
    %a = cir.get_global @w1 : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %c = cir.get_global @w2 : !fnptr
    cir.call @spawn(%c) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in @s2: 'a' (offset 0) and 'b' (offset 8)
// CHECK: false sharing: 1 definite
