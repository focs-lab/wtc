// The compiler's own builds under ThreadSanitizer, instrumented after the
// whole pipeline: the queue it substitutes, inline storage it chooses, and
// the pushes it batches must run without a report. The source is the same
// as in the other tests; what is checked here is the code the compiler
// generated, not a build of the source by another compiler.
//
// REQUIRES: tsan
// RUN: %wtc_cc --tsan -O1 -g %examples/inline_pipeline.cpp -o %t.inline.exe 2>&1 | FileCheck %s --check-prefix=INLINE-BUILD
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.inline.exe 2>&1 | FileCheck %s --check-prefix=INLINE
// RUN: %wtc_cc --tsan -O1 -g %examples/queue_example.cpp -o %t.qe.exe 2>&1 | FileCheck %s --check-prefix=QE-BUILD
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.qe.exe 2>&1 | FileCheck %s --allow-empty --check-prefix=QE
// RUN: %wtc_cc --tsan -O1 -g %examples/batch_loop.cpp -o %t.b2.exe 2>&1 | FileCheck %s --check-prefix=BATCH-BUILD
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.b2.exe 2>&1 | FileCheck %s --check-prefix=B2
// RUN: %wtc_cc --tsan -O1 -g -DTAG=1 %examples/batch_loop.cpp -o %t.b1.exe 2>&1 | FileCheck %s --check-prefix=BATCH-BUILD
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.b1.exe 2>&1 | FileCheck %s --check-prefix=B1
// RUN: %wtc_cc --tsan -O1 -g --no-specialize %examples/batch_loop.cpp -o %t.b0.exe 2>&1 | FileCheck %s --check-prefix=BATCH-BUILD
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.b0.exe 2>&1 | FileCheck %s --check-prefix=B0
//
// INLINE-BUILD: tag rewritten to spsc_inline
// INLINE-NOT: ThreadSanitizer
// INLINE: tag spsc_inline in_order 1
//
// QE-BUILD: queue @q2: mpmc
// QE-BUILD: tag rewritten to spsc_inline
// QE-NOT: ThreadSanitizer
//
// BATCH-BUILD: pushes in this loop batched
// B2-NOT: ThreadSanitizer
// B2: tag 2 in_order 1
// B1-NOT: ThreadSanitizer
// B1: tag 1 in_order 1
// B0-NOT: ThreadSanitizer
// B0: tag 0 in_order 1
