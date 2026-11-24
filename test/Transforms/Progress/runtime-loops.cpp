// The runtime speaks through the progress table, and its own loops are
// not read where a program uses it. Read here, called directly, the loop
// classifier is not more optimistic than the table, for the rows the
// chooser relies on as for the others:
//
//   the bounded ring's push waits for the pop of the previous lap (its
//   first site the fill's wait for the cell's sequence), as its row says;
//   built settled, its row is wait-free with one consumer, and the
//   classifier, which reads both orders of the push (whether it is settled
//   is a field), finds the same wait: more pessimistic, not less;
//   the queue over the SCQ ring, lock-free in its row, is read as a spin
//   lock (a compare-and-swap it skips while what it read says the entry is
//   taken), which is more pessimistic;
//   the one-counter push (jiffy), wait-free in its row, and the
//   general-purpose pop, blocking in its row, first meet the hazard
//   domain's static-initialisation guard (hazard::prepare), which the
//   queue's constructor takes before any operation and which an operation
//   reaches only when no queue has been built, which the classifier
//   cannot tell;
//   the spsc ring's pop, whose row is wait-free with a bounded slip and no
//   first-use effect now that the tick rate is measured when the ring is
//   built, passes.
//
// The wait-free ring of indices is not read: its double-width
// compare-and-swap is not compiled by ClangIR.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: error: wtc: ring_push(int) is declared lock-free but can block
// CHECK-DAG: error: wtc: settled_push(int) is declared lock-free but can block
// CHECK-DAG: error: wtc: scq_push(int) is declared lock-free but can block
// CHECK-DAG: error: wtc: jiffy_push(int) is declared lock-free but can block
// CHECK-DAG: error: wtc: generic_pop(int&) is declared lock-free but can block
// CHECK-DAG: remark: wtc: spsc_pop(int&) is declared lock-free: nothing it reaches blocks
// CHECK-DAG: remark: wtc: spsc_pop(int&): wait-free, finite, not instantiated (a loop over the thread's own data)
// CHECK-DAG: bounded_ring.hpp:{{[0-9]+}}:{{[0-9]+}}: note: which spins until another thread changes what it reads (an atomic or volatile load): it waits for another thread
// CHECK-DAG: index_ring.hpp:{{[0-9]+}}:{{[0-9]+}}: note: which retries a compare-and-swap only when what it read says the location is free, and otherwise skips it: it waits on the value it read, as a spin lock does
// CHECK-DAG: hazard.hpp:{{[0-9]+}}:{{[0-9]+}}: note: which takes a static-initialisation lock (__cxa_guard_acquire)
// CHECK-NOT: error: wtc: spsc_pop

#include "progress.hpp"
#include "../runtime/bounded_ring.hpp"
#include "../runtime/index_ring.hpp"
#include "../runtime/segment_queue.hpp"
#include "../runtime/spsc_ring.hpp"

wtc::rt::bounded_ring<int> ring(3, false, false, false);
wtc::rt::bounded_ring<int> settled(3, false, false, false, false, true);
wtc::rt::index_queue<int> scq(3, true);
wtc::rt::segment_queue<int, false, 1024, 64, wtc::rt::no_hooks, true> jiffy;
wtc::rt::segment_queue<int, true> generic;
wtc::rt::spsc_ring<int> spsc;

WTC_LOCK_FREE bool ring_push(int v) { return ring.try_push(v); }
WTC_LOCK_FREE bool settled_push(int v) { return settled.try_push(v); }
WTC_LOCK_FREE bool scq_push(int v) { return scq.try_push(v); }
WTC_LOCK_FREE void jiffy_push(int v) { jiffy.push(v); }
WTC_LOCK_FREE bool generic_pop(int &v) { return generic.try_pop(v); }
WTC_LOCK_FREE bool spsc_pop(int &v) { return spsc.try_pop(v); }

int main() {
    int v = 0;
    ring_push(1);
    settled_push(1);
    scq_push(1);
    jiffy_push(1);
    generic_pop(v);
    spsc_pop(v);
}
