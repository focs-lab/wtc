// wtc-cc stops at an object file with -c, for a program whose other files
// are built without it and linked with it; the substitution is in the
// object all the same. --wtc-opt-arg reaches wtc-opt, which is how the
// evaluation reads the passes' own timings.
//
// RUN: %wtc_cc -c %examples/inline_pipeline.cpp -o %t.o 2>&1 | FileCheck %s --check-prefix=BUILD
// RUN: %clangxx %t.o -o %t.exe -pthread
// RUN: %t.exe | FileCheck %s
// RUN: %wtc_cc --wtc-opt-arg --mlir-timing %examples/inline_pipeline.cpp -o %t.timed.exe 2>&1 | FileCheck %s --check-prefix=TIMING
//
// BUILD: tag rewritten to spsc_inline
// CHECK: tag spsc_inline in_order 1
// TIMING: Execution time report
// TIMING: Total
