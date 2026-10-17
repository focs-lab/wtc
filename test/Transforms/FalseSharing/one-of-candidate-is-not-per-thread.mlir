// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Workers spawned in a loop, each handed its own element of ss, each
// store that element into one shared field c.t and then hot-write a
// through c.t and b of their own element. The stores run in the workers'
// instances, where the element is the thread's own; to the loading thread
// it is some element, whichever worker stored last. A candidate resolved
// in the storing instance therefore loses its per-thread element and is
// placed at an unknown one -- and since every worker's store names the
// same array, the set is one place, ss at an element nobody can name. It
// used to keep the element, collapse to one certain per-thread place, and
// borrow owner-ness from the thread's other accesses: four definite pairs
// "across elements", none of them real. What remains is the direct pair
// on b, which is real: neighbouring elements of 16 bytes share a line.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!void = !cir.void
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!rec_C = !cir.struct<"C" {data !cir.ptr<!cir.struct<"S" {data !s64i, data !s64i}>>}>
!arr = !cir.array<!rec_S x 8>
!fnptr = !cir.ptr<!cir.func<(!cir.ptr<!rec_S>)>>
module {
  cir.global external @ss = #cir.zero : !arr {alignment = 64 : i64}
  cir.global external @c = #cir.zero : !rec_C {alignment = 8 : i64}
  cir.func @spawn(%fn: !fnptr, %arg: !cir.ptr<!rec_S>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.func internal private @worker(%me: !cir.ptr<!rec_S>) {
    %cp0 = cir.get_global @c : !cir.ptr<!rec_C>
    %tf0 = cir.get_member %cp0[0] {name = "t"} : !cir.ptr<!rec_C> -> !cir.ptr<!cir.ptr<!rec_S>>
    cir.store %me, %tf0 : !cir.ptr<!rec_S>, !cir.ptr<!cir.ptr<!rec_S>>
    cir.for : cond {
      %t = cir.const #cir.bool<true> : !cir.bool
      cir.condition(%t)
    } body {
      %cp = cir.get_global @c : !cir.ptr<!rec_C>
      %tf = cir.get_member %cp[0] {name = "t"} : !cir.ptr<!rec_C> -> !cir.ptr<!cir.ptr<!rec_S>>
      %p = cir.load %tf : !cir.ptr<!cir.ptr<!rec_S>>, !cir.ptr<!rec_S>
      %f = cir.get_member %p[0] {name = "a"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %f : !s64i, !cir.ptr<!s64i>
      %b = cir.get_member %me[1] {name = "b"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      cir.store %v, %b : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      cir.yield
    }
    cir.return
  }
  cir.func @main() {
    %i = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %i : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<8> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %pa = cir.get_global @ss : !cir.ptr<!arr>
      %d = cir.cast array_to_ptrdecay %pa : !cir.ptr<!arr> -> !cir.ptr<!rec_S>
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %k = cir.cast integral %c : !s32i -> !s64i
      %elem = cir.ptr_stride %d, %k : (!cir.ptr<!rec_S>, !s64i) -> !cir.ptr<!rec_S>
      %f = cir.get_global @worker : !fnptr
      cir.call @spawn(%f, %elem) : (!fnptr, !cir.ptr<!rec_S>) -> ()
      cir.yield
    } step {
      %c = cir.load %i : !cir.ptr<!s32i>, !s32i
      %inc = cir.inc nsw %c : !s32i
      cir.store %inc, %i : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
}

// CHECK-NOT: across elements: 'a'
// CHECK: false sharing in @ss (one element per thread) across elements: 'b' (offset 8) of one element and 'b' (offset 8) of the next
// CHECK: false sharing: 1 definite, 0 possible, 0 cold{{.*}}, 1 at array elements the analysis cannot tell apart{{.*}}; 0 accesses placed as one of several objects
