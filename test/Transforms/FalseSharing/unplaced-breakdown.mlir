// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The summary says why each unidentified access could not be placed, so
// that what to place next is read off it: here one access through a
// pointer loaded from a field (`q->next->a`), one through a pointer
// global the module writes twice, and one through a parameter of a
// function nobody in the module calls.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i, data !cir.ptr<!cir.struct<"S">>}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @t = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @gp = #cir.ptr<null> : !cir.ptr<!rec_S>

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func private @worker_a() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %n = cir.get_member %p[2] {name = "next"} : !cir.ptr<!rec_S> -> !cir.ptr<!cir.ptr<!rec_S>>
      %q = cir.load %n : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %a = cir.get_member %q[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %a : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func private @worker_b() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %gpp = cir.get_global @gp : !cir.ptr<!cir.ptr<!rec_S>>
      %q = cir.load %gpp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %b = cir.get_member %q[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %b : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @orphan(%q: !cir.ptr<!rec_S>) {
    %b = cir.get_member %q[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<3> : !s64i
    cir.store %v, %b : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func @main() {
    %gpp = cir.get_global @gp : !cir.ptr<!cir.ptr<!rec_S>>
    %s = cir.get_global @s : !cir.ptr<!rec_S>
    cir.store %s, %gpp : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %t = cir.get_global @t : !cir.ptr<!rec_S>
    cir.store %t, %gpp : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %a = cir.get_global @worker_a : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @worker_b : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: false sharing: 0 definite, 0 possible, 0 cold{{.*}}, 3 field accesses on objects that could not be identified{{.*}}; of the unidentified, 1 through a pointer loaded from a field, 1 from a global written more than once or named elsewhere, 0 from a slot written more than once, 1 through a parameter nobody is seen to pass, 0 from the result of a call that is not an allocation, 0 from an integer, 0 otherwise
