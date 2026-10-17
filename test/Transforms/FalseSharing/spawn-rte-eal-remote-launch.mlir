// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// DPDK's EAL: rte_eal_remote_launch(f, arg, lcore) starts a thread with f
// as its entry, and rte_zmalloc(type, size, align) is an allocation whose
// third operand is its alignment. A program on the EAL has no
// pthread_create and no malloc; without these two names it looked as if it
// never started a thread and placed nothing on its heap.

!s8i = !cir.int<s, 8>
!s32i = !cir.int<s, 32>
!u32i = !cir.int<u, 32>
!s64i = !cir.int<s, 64>
!u64i = !cir.int<u, 64>
!void = !cir.void
!pad = !cir.array<!s64i x 6>
!rec_S = !cir.struct<"S" {data !s64i, data !pad, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @tp = #cir.ptr<null> : !cir.ptr<!rec_S>
  cir.func private @rte_zmalloc(!cir.ptr<!s8i>, !u64i, !u32i) -> !cir.ptr<!void>
  cir.func private @rte_eal_remote_launch(!fnptr, !cir.ptr<!void>, !u32i) -> !s32i
  cir.func internal private @wa() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
      %p = cir.load %tpp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
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
      %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
      %p = cir.load %tpp : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %f = cir.get_member %p[2] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %type = cir.const #cir.ptr<null> : !cir.ptr<!s8i>
    %sz = cir.const #cir.int<64> : !u64i
    %al = cir.const #cir.int<64> : !u32i
    %r = cir.call @rte_zmalloc(%type, %sz, %al) : (!cir.ptr<!s8i>, !u64i, !u32i) -> !cir.ptr<!void>
    %rs = cir.cast bitcast %r : !cir.ptr<!void> -> !cir.ptr<!rec_S>
    %tpp = cir.get_global @tp : !cir.ptr<!cir.ptr<!rec_S>>
    cir.store %rs, %tpp : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    %arg = cir.const #cir.ptr<null> : !cir.ptr<!void>
    %c1 = cir.const #cir.int<1> : !u32i
    %c2 = cir.const #cir.int<2> : !u32i
    %a = cir.get_global @wa : !fnptr
    %ra = cir.call @rte_eal_remote_launch(%a, %arg, %c1) : (!fnptr, !cir.ptr<!void>, !u32i) -> !s32i
    %b = cir.get_global @wb : !fnptr
    %rb = cir.call @rte_eal_remote_launch(%b, %arg, %c2) : (!fnptr, !cir.ptr<!void>, !u32i) -> !s32i
    cir.return
  }
}

// CHECK: wtc: false sharing in rte_zmalloc() in @main: 'a' (offset 0) and 'b' (offset 56){{.*}}share one 64-byte line
// CHECK-NOT: positions
// CHECK: false sharing: 1 definite, 0 possible, 0 cold
