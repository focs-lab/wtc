// RUN: wtc-opt %s --wtc-align-by-owner 2>&1 | FileCheck %s

// Four globals this module does not own outright, each hot-written by one
// thread and paired with a hot global of another. None may be raised:
//
//   @decl   is only declared here; align 64 on a declaration promises what
//           the defining module never gave, and LLVM may rely on it;
//   @sect   sits in a named section, whose __start_/__stop_ walkers step
//           by object size and would land inside the padding;
//   @tls    is thread-local, so every thread already has its own copy;
//   @weak   has linkage under which the linker may keep another module's
//           copy, with that module's alignment.
//
// The earlier code refused only constants and already-aligned globals and
// raised all four. It also reported "from 1" for a global with no alignment
// attribute, where the ABI alignment of an i64 is 8; @plain checks the
// starting point is read from the ABI.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!fnptr = !cir.ptr<!cir.func<()>>
module {
  cir.global "private" external @decl : !s64i
  cir.global external @sect = #cir.int<0> : !s64i {alignment = 8 : i64, section = "mytable"}
  cir.global external tls_model = tls_dyn @tls = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global weak_odr @weak = #cir.int<0> : !s64i {alignment = 8 : i64}
  cir.global external @plain = #cir.int<0> : !s64i
  cir.global external @other = #cir.int<0> : !s64i {alignment = 8 : i64}

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
      %v = cir.const #cir.int<1> : !s64i
      %d = cir.get_global @decl : !cir.ptr<!s64i>
      cir.store %v, %d : !s64i, !cir.ptr<!s64i>
      %s = cir.get_global @sect : !cir.ptr<!s64i>
      cir.store %v, %s : !s64i, !cir.ptr<!s64i>
      %t = cir.get_global thread_local @tls : !cir.ptr<!s64i>
      cir.store %v, %t : !s64i, !cir.ptr<!s64i>
      %w = cir.get_global @weak : !cir.ptr<!s64i>
      cir.store %v, %w : !s64i, !cir.ptr<!s64i>
      %p = cir.get_global @plain : !cir.ptr<!s64i>
      cir.store %v, %p : !s64i, !cir.ptr<!s64i>
      cir.yield
    } step {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %i = cir.inc nsw %c : !s32i
      cir.store %i, %n : !s32i, !cir.ptr<!s32i>
      cir.yield
    }
    cir.return
  }
  cir.func @reader() {
    %n = cir.alloca "i" align(4) : !cir.ptr<!s32i>
    %z = cir.const #cir.int<0> : !s32i
    cir.store %z, %n : !s32i, !cir.ptr<!s32i>
    cir.for : cond {
      %c = cir.load %n : !cir.ptr<!s32i>, !s32i
      %l = cir.const #cir.int<1000> : !s32i
      %lt = cir.cmp lt %c, %l : !s32i
      cir.condition(%lt)
    } body {
      %o = cir.get_global @other : !cir.ptr<!s64i>
      %v = cir.const #cir.int<2> : !s64i
      cir.store %v, %o : !s64i, !cir.ptr<!s64i>
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
    %b = cir.get_global @reader : !fnptr
    cir.call @spawn(%b) : (!fnptr) -> ()
    cir.return
  }
}

// CHECK-DAG: left @decl where it is: it is only declared here
// CHECK-DAG: left @sect where it is: it sits in a named section
// CHECK-DAG: left @tls where it is: it is thread-local
// CHECK-DAG: left @weak where it is: its linkage lets the linker pick another module's copy
// CHECK-DAG: raised the alignment of @plain from 8 to 64
// CHECK-DAG: raised the alignment of @other from 8 to 64
// CHECK: align by owner: 2 globals raised to 64 bytes, at most 112 bytes of padding; refused 0 constant, 0 already aligned, 0 whose partner could not be raised, 4 that cannot move
// CHECK: cir.global "private" external @decl : !s64i{{$}}
// CHECK: @sect = #cir.int<0> : !s64i {alignment = 8 : i64, section = "mytable"}
// CHECK: @tls = #cir.int<0> : !s64i {alignment = 8 : i64}
// CHECK: @weak = #cir.int<0> : !s64i {alignment = 8 : i64}
// CHECK: @plain = #cir.int<0> : !s64i {alignment = 64 : i64}
