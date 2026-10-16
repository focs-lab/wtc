// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two once-writers of field a: one whose thread also has a repeated access
// the walk could not place, and main, whose every access is placed. Field b is hot on
// another thread. The earlier rule stopped at the first writer it could
// vouch for and called the pair cold; the unplaced writer may be writing a
// through the pointer the walk could not follow, so the pair is possible,
// whatever the other writers look like.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
!rec_T = !cir.struct<"T" {data !s64i}>
module {
  cir.global external @gp = #cir.ptr<null> : !cir.ptr<!rec_T>
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %a : !s64i, !cir.ptr<!s64i>
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
      %b = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %b : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @wab() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %b = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %a : !s64i, !cir.ptr<!s64i>
      cir.store %v, %b : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @w1() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %gpp = cir.get_global @gp : !cir.ptr<!cir.ptr<!rec_T>>
      %tp = cir.load %gpp : !cir.ptr<!cir.ptr<!rec_T>>, !cir.ptr<!rec_T>
      %tm = cir.get_member %tp[0] {name = "t"} : !cir.ptr<!rec_T> -> !cir.ptr<!s64i>
      %tv = cir.const #cir.int<3> : !s64i
      cir.store %tv, %tm : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %y = cir.get_global @w1 : !fnptr
    cir.call @spawn(%y) : (!fnptr) -> ()
    %x = cir.get_global @wb : !fnptr
    cir.call @spawn(%x) : (!fnptr) -> ()
    %p2 = cir.get_global @s : !cir.ptr<!rec_S>
    %a2 = cir.get_member %p2[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v2 = cir.const #cir.int<1> : !s64i
    cir.store %v2, %a2 : !s64i, !cir.ptr<!s64i>
    cir.return
  }
}

// CHECK-NOT: wtc: false sharing in
// CHECK: possible false sharing in @s: 'a' and 'b'{{.*}}accesses it could not place
// CHECK: false sharing: 0 definite, 1 possible, 0 cold{{.*}}1 of the possible pairs would be cold but for unplaced accesses
