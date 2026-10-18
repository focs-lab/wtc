// REQUIRES: clangir-x86-64
// RUN: %cir_x86_64_cxx %S/queue-two-queues.cpp -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null > %t.out
// RUN: FileCheck %S/queue-two-queues.cpp --check-prefix=Q1 < %t.out
// RUN: FileCheck %S/queue-two-queues.cpp --check-prefix=Q2 < %t.out

// queue-two-queues.cpp, emitted for x86-64. There, spawn and try_pop return
// their records through a hidden first argument, which on AArch64 they do
// not: this is the shape that made lifting segfault and the analysis find
// no threads. The program and the expected verdicts are that file's.
