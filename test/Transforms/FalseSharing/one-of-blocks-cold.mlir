// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A thread writes `s.a` once, directly, and in a loop writes field b
// through a pointer that may name s or t. Against another thread's hot
// read of `s.b`, the once-only write of a would be cold -- but the same
// thread may be hot-writing s through the set, so the pair is possible,
// on the same terms as a thread with an unplaced access. The access
// through the set is placed, and the summary says so.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @t = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @flag = #cir.int<0> : !s32i
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa() {
    %slot = cir.alloca "p" align(8) : !cir.ptr<!cir.ptr<!rec_S>>
    %ps0 = cir.get_global @s : !cir.ptr<!rec_S>
    %fa = cir.get_member %ps0[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v0 = cir.const #cir.int<7> : !s64i
    cir.store %v0, %fa : !s64i, !cir.ptr<!s64i>
    %fp = cir.get_global @flag : !cir.ptr<!s32i>
    %fv = cir.load %fp : !cir.ptr<!s32i>, !s32i
    %c = cir.cast int_to_bool %fv : !s32i -> !cir.bool
    cir.if %c {
      %ps = cir.get_global @s : !cir.ptr<!rec_S>
      cir.store %ps, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
      cir.yield
    } else {
      %pt = cir.get_global @t : !cir.ptr<!rec_S>
      cir.store %pt, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
      cir.yield
    }
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.load %slot : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %f = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
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
      %ps = cir.get_global @s : !cir.ptr<!rec_S>
      %fb = cir.get_member %ps[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %r = cir.load %fb : !cir.ptr<!s64i>, !s64i
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: possible false sharing in @s: 'a' and 'b' share one 64-byte line{{.*}}; one side touches it once as far as the analysis can see, but that thread also has accesses it could not place, or placed only as one of several objects
// CHECK: false sharing: 0 definite, 1 possible, 0 cold{{.*}}, 0 field accesses on objects that could not be identified{{.*}}; 1 of the possible pairs would be cold but for unplaced accesses{{.*}}; 1 accesses placed as one of several objects
