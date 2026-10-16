// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// f(&s) writes a field of its argument and calls f(&s2). The recursive call
// is where s2 is written, and the walk used to skip it, so s2.a was never
// recorded and the pair with s2.b, hot on another thread, was missed. A
// recursive instance is visited; only its expansion stops.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
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
  cir.global external @s2 = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func internal private @f(%q: !cir.ptr<!rec_S>, %c: !cir.bool) {
    %a = cir.get_member %q[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.if %c {
      %o = cir.get_global @s2 : !cir.ptr<!rec_S>
      %f = cir.const #cir.bool<false> : !cir.bool
      cir.call @f(%o, %f) : (!cir.ptr<!rec_S>, !cir.bool) -> ()
    }
    cir.return
  }
  cir.func internal private @w1() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %t = cir.const #cir.bool<true> : !cir.bool
    cir.call @f(%p, %t) : (!cir.ptr<!rec_S>, !cir.bool) -> ()
    cir.return
  }
  cir.func internal private @w2() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.get_global @s2 : !cir.ptr<!rec_S>
      %b = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %b : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %w = cir.get_global @w1 : !fnptr
    cir.call @spawn(%w) : (!fnptr) -> ()
    %x = cir.get_global @w2 : !fnptr
    cir.call @spawn(%x) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: wtc: false sharing in @s2: 'a' (offset 0) and 'b' (offset 8)
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
