// The original example again, compiled at -O1. The annotations the whole
// mechanism depends on are carried on the function declarations, and the
// worry was that inlining would substitute the shim away along with them.
// It does not: -emit-cir stops before the middle end, so nothing has been
// inlined yet at either level. This test is what keeps that true, since a
// change in where annotations are dropped would otherwise show up first as
// a program that silently stops being specialised.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -O1 -emit-cir %examples/queue_example.cpp -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize 2>&1 >%t.wtc.cir | FileCheck %s
//
// The same two verdicts as at -O0, and the same single rewrite.
// CHECK-DAG: queue @q1: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @q2: mpmc (producers: many, consumers: many)
// CHECK: specialised: constructor tag rewritten to spsc
// CHECK-NOT: specialised: constructor tag rewritten
