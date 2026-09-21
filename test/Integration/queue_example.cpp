// The original prototype program, through the whole pipeline from source.
// q1 has one producer and one consumer; q2 has two of each. The compiler
// must prove the first, refuse the second, rewrite exactly one constructor
// tag, and the resulting binary must run correctly under the contract check.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %examples/queue_example.cpp -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize 2>&1 >%t.wtc.cir | FileCheck %s --check-prefix=REMARKS
// RUN: FileCheck %s --check-prefix=IR < %t.wtc.cir
// RUN: grep -B1 'ItemEC[12]ENS_8topologyE' %t.wtc.cir | FileCheck %s --check-prefix=CTOR
// RUN: %wtc_cc -DWTC_CHECK %examples/queue_example.cpp -o %t.exe
// RUN: %t.exe
//
// The verdicts, in module order: q1 first, q2 second.
// REMARKS-DAG: queue @q1: spsc (producers: 1, consumers: 1)
// REMARKS-DAG: queue @q2: mpmc (producers: many, consumers: many)
// REMARKS: specialised: constructor tag rewritten to spsc
// REMARKS-NOT: specialised: constructor tag rewritten
//
// The globals carry the verdict, and only q1 carries the mark of a rewrite.
// IR-DAG: cir.global{{.*}}@q1{{.*}}wtc.specialized = #wtc.topology<spsc>{{.*}}wtc.topology = #wtc.topology<spsc>
// IR-DAG: cir.global{{.*}}@q2{{.*}}wtc.topology = #wtc.topology<mpmc>
// IR-NOT: @q2{{.*}}wtc.specialized
//
// The two constructor calls, each with the constant that feeds its tag on
// the line before: q1's rewritten to 1, q2's left at 0. A plain search for
// the constant 1 would match any loop counter in the program.
// CTOR: cir.const #cir.int<1> : !s32i
// CTOR-NEXT: cir.call @_ZN3wtc5queueI4ItemEC{{[12]}}ENS_8topologyE
// CTOR: cir.const #cir.int<0> : !s32i
// CTOR-NEXT: cir.call @_ZN3wtc5queueI4ItemEC{{[12]}}ENS_8topologyE
