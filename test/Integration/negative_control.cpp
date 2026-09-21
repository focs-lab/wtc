// The test that proves the contract check can fire. q2 has two producers and
// two consumers; forcing it spsc builds a program that races, and with
// WTC_CHECK the ring must abort on the first foreign thread. Without this
// test a check that never fires would look exactly like a correct program.
//
// `not --crash`, not plain `not`: the check ends the program with abort,
// which is a signal, and plain `not` counts a signal as a crash rather than
// as the non-zero exit it was asked to expect.
//
// RUN: %wtc_cc -DWTC_CHECK --force-spsc q2 %examples/queue_example.cpp -o %t.exe
// RUN: not --crash %t.exe 2>&1 | FileCheck %s
//
// CHECK: wtc: SPSC contract violated: a second {{producer|consumer}} thread on queue
