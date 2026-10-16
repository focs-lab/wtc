// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// The same two threads writing the same struct, but the author already put
// 64 bytes between the fields. There is nothing to report, and reporting it
// anyway would be the failure mode that makes a checker useless.

!s64i = !cir.int<s, 64>
!pad = !cir.array<!s64i x 8>
!rec_P = !cir.struct<"P" {data !s64i, data !pad, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @p = #cir.zero : !rec_P {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @worker_a() {
    %b = cir.get_global @p : !cir.ptr<!rec_P>
    %m = cir.get_member %b[0] {name = "hits"} : !cir.ptr<!rec_P> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %m : !s64i, !cir.ptr<!s64i>
    cir.return
  }

  cir.func @worker_b() {
    %b = cir.get_global @p : !cir.ptr<!rec_P>
    %m = cir.get_member %b[2] {name = "misses"} : !cir.ptr<!rec_P> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<2> : !s64i
    cir.store %v, %m : !s64i, !cir.ptr<!s64i>
    cir.return
  }

  cir.func @main() {
    %a = cir.get_global @worker_a : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %c = cir.get_global @worker_b : !fnptr
    cir.call @spawn(%c) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: remark: wtc: false sharing in @p
// CHECK: false sharing: 0 definite, 0 possible
