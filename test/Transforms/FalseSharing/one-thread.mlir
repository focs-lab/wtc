// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Both fields written, both by main. One thread cannot contend with itself,
// and padding here would cost memory for nothing.

!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @main() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %m0 = cir.get_member %p[0] {name = "hits"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %m1 = cir.get_member %p[1] {name = "misses"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %m0 : !s64i, !cir.ptr<!s64i>
    cir.store %v, %m1 : !s64i, !cir.ptr<!s64i>
    cir.return
  }
}

// CHECK-NOT: remark: wtc: {{(possible )?}}false sharing in
// CHECK: false sharing: 0 definite, 0 possible, 0 cold
