// RUN: wtc-opt %s --wtc-false-sharing-diagnose=max-instances=120 2>&1 >/dev/null | FileCheck %s

// Four functions that each call the other three under a condition: a
// group of mutually recursive functions, as a recursive-descent parser
// has. Unrolling every cycle once per path made the walk exponential in
// the group's size -- 101,574 instances for five functions, and the cap
// for six -- so a recurrence below a recurrence is not expanded. With the
// cap at 120 the walk must still finish (it takes 94 instances) and give its verdict: the
// group is recursive, so its store repeats, and the pair is definite.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u8i = !cir.int<u, 8>
!u64i = !cir.int<u, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wb() {
    cir.for : cond {
      %t_n = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t_n)
    } body {
      %p_wb = cir.get_global @s : !cir.ptr<!rec_S>
      %f_wb = cir.get_member %p_wb[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v_wb = cir.const #cir.int<2> : !s64i
      cir.store %v_wb, %f_wb : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @f0(%c: !cir.bool) {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.if %c {
      cir.call @f1(%c) : (!cir.bool) -> ()
      cir.call @f2(%c) : (!cir.bool) -> ()
      cir.call @f3(%c) : (!cir.bool) -> ()
      cir.yield
    }
    cir.return
  }
  cir.func internal private @f1(%c: !cir.bool) {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.if %c {
      cir.call @f0(%c) : (!cir.bool) -> ()
      cir.call @f2(%c) : (!cir.bool) -> ()
      cir.call @f3(%c) : (!cir.bool) -> ()
      cir.yield
    }
    cir.return
  }
  cir.func internal private @f2(%c: !cir.bool) {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.if %c {
      cir.call @f0(%c) : (!cir.bool) -> ()
      cir.call @f1(%c) : (!cir.bool) -> ()
      cir.call @f3(%c) : (!cir.bool) -> ()
      cir.yield
    }
    cir.return
  }
  cir.func internal private @f3(%c: !cir.bool) {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.if %c {
      cir.call @f0(%c) : (!cir.bool) -> ()
      cir.call @f1(%c) : (!cir.bool) -> ()
      cir.call @f2(%c) : (!cir.bool) -> ()
      cir.yield
    }
    cir.return
  }
  cir.func internal private @w1() {
    %t = cir.const #cir.bool<true> : !cir.bool
    cir.call @f0(%t) : (!cir.bool) -> ()
    cir.return
  }
  cir.func @main() {
    %a = cir.get_global @w1 : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: gave up
// CHECK: wtc: false sharing in @s: 'a' (offset 0) and 'b' (offset 8)
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
