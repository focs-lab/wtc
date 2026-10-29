// wtc-cc stops at an object file with -c, for a program whose other files
// are built without wtc-cc and linked with it. Those files may name any of
// this one's external symbols, so the example's queue, a global with
// external linkage, escapes and keeps the general-purpose implementation
// in the object (BUILD, CHECK); built and linked by wtc-cc alone, the same
// file is the whole program and the substitution is made (WHOLE).
// --wtc-opt-arg reaches wtc-opt, which is how the evaluation reads the
// passes' own timings.
//
// RUN: %wtc_cc -c %examples/inline_pipeline.cpp -o %t.o 2>&1 | FileCheck %s --check-prefix=BUILD
// RUN: %clangxx %t.o -o %t.exe -pthread
// RUN: %t.exe | FileCheck %s
// RUN: %wtc_cc --wtc-opt-arg --mlir-timing %examples/inline_pipeline.cpp -o %t.timed.exe 2>&1 | FileCheck %s --check-prefix=TIMING
// RUN: %t.timed.exe | FileCheck %s --check-prefix=WHOLE
//
// BUILD: wtc: a queue escapes this translation unit (it is reachable from the global q, and the unit is built to be linked with other files of the program)
// BUILD-NOT: tag rewritten
// CHECK: tag generic in_order 1
// TIMING: tag rewritten to spsc_inline
// TIMING: Execution time report
// TIMING: Total
// WHOLE: tag spsc_inline in_order 1
