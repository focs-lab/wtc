// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The textbook case: two counters in one struct, each bumped by its own
// thread with an atomic fetch-add. No load, no store -- the read-modify-
// write is one operation -- and the pass accepted loads and stores only,
// so this program produced no verdict and not even a summary line. The
// third field is written with an exchange and the fourth with a bit-field
// store, which are writes for the same reason.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!u16i = !cir.int<u, 16>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i, data !s64i, data !u16i}>
!fnptr = !cir.ptr<!cir.func<()>>
#bfi_f = #cir.bitfield_info<name = "f", storage_type = !u16i, size = 3, offset = 0, is_signed = false>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func private @bump_a() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %one = cir.const #cir.int<1> : !s64i
      %old = cir.atomic.fetch add relaxed syncscope(system) fetch_first %a, %one : (!cir.ptr<!s64i>, !s64i) -> !s64i
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func private @bump_rest() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %one = cir.const #cir.int<1> : !s64i
      %b = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %old = cir.atomic.fetch add relaxed syncscope(system) fetch_first %b, %one : (!cir.ptr<!s64i>, !s64i) -> !s64i
      %c3 = cir.get_member %p[2] {name = "c"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %prev = cir.atomic.xchg relaxed syncscope(system) %c3, %one : (!cir.ptr<!s64i>, !s64i) -> !s64i
      %f = cir.get_member %p[3] {name = "f"} : !cir.ptr<!rec_S> -> !cir.ptr<!u16i>
      %v = cir.const #cir.int<5> : !s32i
      %set = cir.set_bitfield align(2) (#bfi_f, %f : !cir.ptr<!u16i>, %v : !s32i) -> !s32i
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  cir.func @main() {
    %a = cir.get_global @bump_a : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @bump_rest : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-DAG: false sharing in @s: 'a' (offset 0) and 'b' (offset 8)
// CHECK-DAG: false sharing in @s: 'a' (offset 0) and 'c' (offset 16)
// CHECK-DAG: false sharing in @s: 'a' (offset 0) and 'f' (offset 24)
// CHECK: false sharing: 3 definite, 0 possible, 0 cold
