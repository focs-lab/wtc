// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 | FileCheck %s
// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s --check-prefix=DIAG

// A global declared with an incomplete record type, which the program only
// ever casts to something it can write through. Neither pass can place
// anything in it, and asking the data layout for its size asserts. Both
// passes used to crash here; now the alignment pass leaves it alone and the
// diagnosis counts its accesses as unidentified.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_Opaque = !cir.struct<"Opaque" incomplete>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @opaque : !rec_Opaque
  cir.global external @y = #cir.int<0> : !s64i {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @writer() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %o = cir.get_global @opaque : !cir.ptr<!rec_Opaque>
      %s = cir.cast bitcast %o : !cir.ptr<!rec_Opaque> -> !cir.ptr<!rec_S>
      %f = cir.get_member %s[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @other() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %y = cir.get_global @y : !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %y : !s64i, !cir.ptr<!s64i>
      %o = cir.get_global @opaque : !cir.ptr<!rec_Opaque>
      %s = cir.cast bitcast %o : !cir.ptr<!rec_Opaque> -> !cir.ptr<!rec_S>
      %f = cir.get_member %s[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
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
    %a = cir.get_global @writer : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @other : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: left @opaque where it is: it is only declared here
// CHECK: align by owner: 0 globals raised
// CHECK-NOT: alignment = 64

// DIAG-NOT: false sharing in
// DIAG: false sharing: 0 definite, 0 possible, 0 cold{{.*}}, 2 field accesses on objects that could not be identified
