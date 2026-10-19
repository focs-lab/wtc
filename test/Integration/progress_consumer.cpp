// The progress check end to end, on one consumer whose per-element step is
// declared lock-free, in the four builds its source describes.
//
// RUN: %wtc_cc %examples/progress_consumer.cpp -o %t.exe 2>&1 | FileCheck %s --check-prefix=INLINE
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: not %wtc_cc --no-specialize %examples/progress_consumer.cpp -o %t.generic.exe 2>&1 | FileCheck %s --check-prefix=GENERIC
// RUN: not %wtc_cc -DRETAIN %examples/progress_consumer.cpp -o %t.retain.exe 2>&1 | FileCheck %s --check-prefix=RETAIN
// RUN: not %wtc_cc -DLOG %examples/progress_consumer.cpp -o %t.log.exe 2>&1 | FileCheck %s --check-prefix=LOG
//
// INLINE: tag rewritten to spsc_inline
// INLINE: remark: wtc: consume_one() is declared lock-free: nothing it reaches blocks
// OUT: total 4999950000
//
// GENERIC: error: wtc: consume_one() is declared lock-free but can block
// GENERIC: note: which takes the queue's mutexes and frees the node it unlinks
//
// RETAIN: queue @q: not borrowed
// RETAIN: error: wtc: consume_one() is declared lock-free but can block
// RETAIN: note: calls std::shared_ptr<sample>::operator=
//
// LOG: error: wtc: consume_one() is declared lock-free but can block
// LOG: note: which does input or output (printf)
