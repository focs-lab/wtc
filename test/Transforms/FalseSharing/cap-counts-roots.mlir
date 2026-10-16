// RUN: wtc-opt %s --wtc-false-sharing-diagnose=max-instances=2 2>&1 >/dev/null | FileCheck %s

// Three functions whose addresses sit in a table, so each is a root that
// may run on any thread, plus main: four instances against a cap of two.
// Roots used to be added without consulting the cap, so this module got a
// verdict over four instances and no word that the cap had been passed.
// The cap bounds the walk or it bounds nothing; past it the pass gives up,
// and the "address taken" remarks that would have preceded the verdict are
// held back too, since there is no verdict for them to explain.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
!rec_T = !cir.struct<"T" {data !fnptr, data !fnptr, data !fnptr}>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @table = #cir.const_record<{#cir.global_view<@h0> : !fnptr, #cir.global_view<@h1> : !fnptr, #cir.global_view<@h2> : !fnptr}> : !rec_T

  cir.func private @h0() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func private @h1() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %b = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<2> : !s64i
    cir.store %v, %b : !s64i, !cir.ptr<!s64i>
    cir.return
  }
  cir.func private @h2() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %b = cir.get_member %p[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<3> : !s64i
    cir.store %v, %b : !s64i, !cir.ptr<!s64i>
    cir.return
  }

  cir.func @main() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %a = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<0> : !s64i
    cir.store %v, %a : !s64i, !cir.ptr<!s64i>
    cir.return
  }
}

// CHECK-NOT: address of this function is taken
// CHECK-NOT: false sharing:
// CHECK: gave up after 2 function instances
