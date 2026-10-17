// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// A pointer local assigned once, inside the `cir.cleanup.scope` the front
// end wraps a declaration in at -O2 and bracketed by lifetime markers, and
// read in a loop after it: the one store runs before every load, as surely
// as a store in the slot's own block would, so the walk follows it to the
// object and the pair is definite. Before, the store in a nested scope and
// the lifetime marker each made the slot "written more than once" and the
// accesses through it unplaced: on DPDK's stack test that was every access
// to the stack.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @wa() {
    %slot = cir.alloca "p" align(8) : !cir.ptr<!cir.ptr<!rec_S>>
    cir.cleanup.scope {
      cir.lifetime.start %slot : !cir.ptr<!cir.ptr<!rec_S>>
      cir.cleanup.scope {
        %x = cir.get_global @s : !cir.ptr<!rec_S>
        cir.store %x, %slot : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
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
        cir.yield
      } cleanup normal {
        cir.lifetime.end %slot : !cir.ptr<!cir.ptr<!rec_S>>
        cir.yield
      }
      cir.yield
    } cleanup normal {
      cir.yield
    }
    cir.return
  }
  cir.func internal private @wb() {
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %ps = cir.get_global @s : !cir.ptr<!rec_S>
      %fs = cir.get_member %ps[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %vs = cir.const #cir.int<2> : !s64i
      cir.store %vs, %fs : !s64i, !cir.ptr<!s64i>
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

// CHECK: wtc: false sharing in @s: 'a' (offset 0) and 'b' (offset 8) are touched by different threads
// CHECK: false sharing: 1 definite, 0 possible, 0 cold{{.*}}, 0 field accesses on objects that could not be identified
