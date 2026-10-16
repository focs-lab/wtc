// RUN: wtc-opt %s --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

// main sets one field while parsing options, before any thread exists. A
// handler registered through a table -- a command that changes a setting --
// rewrites another field of the same struct, and since its address is
// merely taken it may run on any thread, any number of times.
//
// The start-up write happens once, before there is anyone to contend with,
// so whatever the handler does, the line never travels on its account.
// Nothing is known about the handler's thread, and nothing needs to be:
// the pair is not `possible`.
//
// It was. `possible` was computed from whether a field had ever been
// written, by anyone, at any time, and on memcached that paired every
// option the getopt loop sets with the command handlers that rewrite a
// setting later. `cold` does not catch it, because cold is a resolved
// claim and needs both threads accounted for; the handler's is not.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!rec_S = !cir.struct<"S" {data !s64i, data !s64i}>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global external @s = #cir.zero : !rec_S {alignment = 8 : i64}
  cir.global external @on_command = #cir.global_view<@set_limit> : !fnptr

  cir.func @set_limit() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %m = cir.get_member %p[1] {name = "limit"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<1> : !s64i
    cir.store %v, %m : !s64i, !cir.ptr<!s64i>
    cir.return
  }

  cir.func @main() {
    %p = cir.get_global @s : !cir.ptr<!rec_S>
    %m = cir.get_member %p[0] {name = "maxbytes"} : !cir.ptr<!rec_S> -> !cir.ptr<!s64i>
    %v = cir.const #cir.int<64> : !s64i
    cir.store %v, %m : !s64i, !cir.ptr<!s64i>
    cir.return
  }
}

// CHECK-NOT: possible false sharing
// CHECK: false sharing: 0 definite, 0 possible, 0 cold
