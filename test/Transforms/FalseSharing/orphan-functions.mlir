// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// One file of a program that has many. Neither function is called here, so
// each becomes a placeholder for "some caller somewhere" and gets its own
// thread identifier. Two placeholders are not two threads, and pairing them
// is not a finding.
//
// This refusal was found, not designed: scanning memcached one file at a
// time, the pass reported 527 definite pairs, essentially all of them this
// artefact. The one-translation-unit assumption is written down in the
// project's documents; this is what it costs when it is violated, and the
// analysis has to say so itself rather than rely on the reader remembering.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @touch_hits() {
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
      %m = cir.get_member %p[0] {name = "hits"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }

  cir.func @touch_misses() {
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
      %m = cir.get_member %p[1] {name = "misses"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
}

// The summary is asserted whole, not just its first field: "0 definite"
// alone was satisfied by whichever refusal happened to swallow the pair, so
// the test could not tell the placeholder rule from the cold rule and would
// have passed with the placeholder rule deleted.
// CHECK: false sharing: 0 definite, 1 possible, 0 cold
