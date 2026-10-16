// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two threads writing the *same* field. The line does travel, but moving
// the field cannot help: this is true sharing, a race or a counter the
// program synchronises, and either way not a layout question. A pass that
// reported it would be sending the programmer to fix the wrong thing.

!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @worker_a() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %m = cir.get_member %p[0] {name = "hits"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %m : !s64i, !cir.ptr<!s64i>
    cir.return
  }

  cir.func @worker_b() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %m = cir.get_member %p[0] {name = "hits"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<2> : !s64i
    cir.store %v, %m : !s64i, !cir.ptr<!s64i>
    cir.return
  }

  cir.func @main() {
    %a = cir.get_global @worker_a : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @worker_b : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: remark: wtc: {{(possible )?}}false sharing in
// CHECK: false sharing: 0 definite, 0 possible, 0 cold
