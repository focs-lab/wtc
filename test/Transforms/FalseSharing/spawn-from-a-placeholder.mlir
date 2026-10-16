// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two externally visible functions nobody here calls, each starting a
// thread. The functions are placeholders for callers in another translation
// unit; a thread started by a placeholder is no more accounted for than the
// placeholder is.
//
// It was. Spawn children were constructed with the speculative flag hard
// coded to false while ordinary calls inherited it, so the artefact that
// produced 527 findings on memcached came back one level down: two
// placeholders spawning gave two accounted-for threads and a definite pair.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @worker_a() {
    %na = cir.alloca "ia" align(4) : !cir.ptr<!s32i>
    %za = cir.const #cir.int<0> : !s32i
    cir.store %za, %na : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %na : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[0] {name = "hits"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %na : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %na : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @worker_b() {
    %nb = cir.alloca "ib" align(4) : !cir.ptr<!s32i>
    %zb = cir.const #cir.int<0> : !s32i
    cir.store %zb, %nb : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %nb : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[1] {name = "misses"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %nb : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %nb : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  // Neither is called here: both are the module's edge.
  cir.func @start_reader() {
    %a = cir.get_global @worker_a : !fnptr
    cir.call @spawn(%a) : (!fnptr) -> ()
    cir.return
  }
  cir.func @start_writer() {
    %b = cir.get_global @worker_b : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-NOT: wtc: false sharing in @s
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
