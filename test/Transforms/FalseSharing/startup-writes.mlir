// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// main fills a configuration struct in a loop and only then starts a worker
// that reads a neighbouring field, also in a loop. Both sides repeat, the
// fields share a line, and the threads differ -- and there is still nothing
// to report, because the writes finish before the reader exists.
//
// Found on memcached: the getopt loop writing settings.maxbytes and its
// neighbours paired against every worker that later reads the same struct,
// for 176 findings that were one start-up phase. Almost every program writes
// a great deal of shared state while starting and never again; an analysis
// without a notion of that phase reports the whole of it.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @cfg = #cir.zero : !rec_S {alignment = 8 : i64}

  cir.func @spawn(%fn: !fnptr) [#cir.annotation<"wtc_thread_spawn">] { cir.return }

  cir.func @worker() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @cfg : !cir.ptr<!rec_S>
      %m = cir.get_member %p[1] {name = "limit"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
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

  cir.func @main() {
    // Option parsing: repeated, but before any thread exists.
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %zero = cir.const #cir.int<0> : !s32i
    cir.store %zero, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %lim = cir.const #cir.int<8> : !s32i
      %lt = cir.cmp lt %c, %lim : !s32i
      cir.condition(%lt)
    } body {
      %p = cir.get_global @cfg : !cir.ptr<!rec_S>
      %m = cir.get_member %p[0] {name = "maxbytes"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
      %v = cir.const #cir.int<1> : !s64i
      cir.store %v, %m : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    %w = cir.get_global @worker : !fnptr
    cir.call @spawn(%w) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK: false sharing: 0 definite, 0 possible, 1 cold
