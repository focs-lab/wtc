// A queue the program declares spsc_inline goes through the whole pipeline:
// the specialisation pass leaves a declared tag alone, the elements are
// held in the ring, and every element arrives once and in order. With
// WTC_CHECK the same program runs with the guard that catches a borrowed
// element kept past the next pop, and must stay silent.
//
// RUN: %wtc_cc -DDECLARE_INLINE %examples/inline_pipeline.cpp -o %t.exe
// RUN: %t.exe | FileCheck %s
// RUN: %wtc_cc -DDECLARE_INLINE -DWTC_CHECK %examples/inline_pipeline.cpp -o %t.check.exe
// RUN: %t.check.exe | FileCheck %s
//
// 3 * (0 + 1 + ... + 199999) = 59999700000
// CHECK: tag spsc_inline in_order 1 sum 59999700000
