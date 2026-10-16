// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// Two threads, two fields, one line -- and no cost. The writer stores once,
// on a shutdown path, so the line is invalidated once in the life of the
// program. A line only travels for as long as both sides keep touching it.
//
// This refusal was not designed, it was found: run on memcached, the pass
// reported `settings.lru_maintainer_thread` against `settings.lru_crawler`,
// and the write turned out to be `settings.lru_maintainer_thread = false` in
// stop_lru_maintainer_thread. One store. Padding it would buy nothing and
// cost 64 bytes, and reporting it would send a programmer to the wrong
// place -- which is the failure the neighbouring project measured and named,
// that the sites a static analysis finds are dynamically cold.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  // Reads its field in a loop: hot.
  cir.func @poller() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @s : !cir.ptr<!rec_S>
      %m = cir.get_member %p[0] {name = "enabled"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.load %m : !cir.ptr<!s64i>, !s64i
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  // Writes the neighbouring field exactly once: cold.
  cir.func @shutdown() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %m = cir.get_member %p[1] {name = "stopping"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %m : !s64i, !cir.ptr<!s64i>
    cir.return
  }

  cir.func @main() {
    %w = cir.get_global @poller : !fnptr
    cir.call @spawn(%w) : (!fnptr) -> ()
    cir.call @shutdown() : () -> ()
    cir.return
  }
}

// CHECK: false sharing: 0 definite, 0 possible, 1 cold
