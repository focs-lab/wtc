// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// one-of-keeps-offset with 64-byte elements in a line-aligned array: the
// candidate `&arr[1]` carries 64 bytes of offset, so the write through
// the pointer lands at 64 and shares a line with `arr[1].b` at 72. Had
// the offset been dropped the write would be at 0, on another line, and
// no pair would be reported. With 16-byte elements (the committed test)
// both placements are on line 0 and the test cannot tell them apart.

!s8i = !cir.int<s, 8>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i, data !cir.array<!s8i x 48>}>
!arr = !cir.array<!rec_S x 4>
!s32i = !cir.int<s, 32>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @arr = #cir.zero : !arr {alignment = 64 : i64}
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @flag = #cir.int<0> : !s32i
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa() {
    %slot = cir.alloca "p" align(8) : !cir.ptr<!cir.ptr<!rec_S>>
    %fp = cir.get_global @flag : !cir.ptr<!s32i>
    %fv = cir.load %fp : !cir.ptr<!s32i>, !s32i
    %c = cir.cast int_to_bool %fv : !s32i -> !cir.bool
    cir.if %c {
      %ps = cir.get_global @s : !cir.ptr<!rec_S>
      cir.store %ps, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
      cir.yield
    } else {
      %pa = cir.get_global @arr : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %pa : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %one = cir.const #cir.int<1> : !s64i
      %e = cir.ptr_stride %d, %one : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      cir.store %e, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
      cir.yield
    }
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %p = cir.load %slot : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %f = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
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
      %pa = cir.get_global @arr : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %pa : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %one = cir.const #cir.int<1> : !s64i
      %e = cir.ptr_stride %d, %one : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %f = cir.get_member %e[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %a = cir.get_global @wa : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    %b = cir.get_global @wb : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: possible false sharing in @arr: 'a' and '[1].b' share one 64-byte line and are touched repeatedly, but through a pointer that may name one of several objects, this among them
// CHECK: false sharing: 0 definite, 1 possible, 0 cold{{.*}}; 1 accesses placed as one of several objects
