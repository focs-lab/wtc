// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The entry reaches the spawn through an integer round trip, which the
// spawn cannot resolve; the function is then one whose address is taken,
// a root the module cannot account for, and the pair is possible. The
// forward check used to follow every cast and declare the function a
// spawn entry, so it was neither resolved nor a root, and its thread was
// lost: the pair came out cold.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
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
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %b : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %a0 = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %z = cir.const #cir.int<0> : !s64i
    cir.store %z, %a0 : !s64i, !cir.ptr<!s64i>
    %fa = cir.get_global @wa : !fnptr
    %i = cir.cast ptr_to_int %fa : !fnptr -> !u64i
    %fa2 = cir.cast int_to_ptr %i : !u64i -> !fnptr
    cir.call @spawn(%fa2) : (!fnptr) -> ()
    %fb = cir.get_global @wb : !fnptr
    cir.call @spawn(%fb) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: wtc: false sharing in
// CHECK: possible false sharing in @s: 'a' and 'b'{{.*}}a thread the analysis cannot account for
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
