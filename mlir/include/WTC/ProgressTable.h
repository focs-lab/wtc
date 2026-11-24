#ifndef WTC_PROGRESSTABLE_H
#define WTC_PROGRESSTABLE_H

// The progress of every wtc operation, by its form, the implementation
// its object runs and the thread counts the analysis proved for that
// object (docs/progress.md). One table, which ProgressOpInterface answers
// from and --wtc-progress-table prints, so that what the decisions say
// and what the passes check cannot drift apart again.
//
// Read from the runtime's code (runtime/, at the revision this compiler
// ships with). Six implementations serve only a path's requirement: scq
// and wcq, the bounded rings of indices of "A scalable, portable, and
// memory-efficient lock-free FIFO queue" (DISC'19) and "wCQ: a fast
// wait-free queue with bounded memory usage" (SPAA'22); jiffy, the mpsc
// queue whose push takes its index from one counter, as in "Jiffy: a fast,
// memory efficient, wait-free multi-producers single-consumer queue"; and
// the settled bounded rings, spsc_settled, mpsc_settled and spmc_settled.
// Each is chosen only when the runtime the program is built with announces
// it (WTCSpecialize.cpp, runtimeOffers).

#include "WTC/Progress.h"
#include "WTC/WTCAttrs.h"

namespace mlir::wtc {

ProgressSpec progressSpecFor(OpForm form, Impl impl, const ThreadCounts &counts);

// The implementations a form can run on: every one for a queue form,
// generic alone (it is ignored) for the rest.
llvm::SmallVector<Impl, 8> implsFor(OpForm form);

// The counts at which a row may differ, for the table: an implementation
// that serves one producer (or one consumer) only is printed at that
// count alone.
llvm::SmallVector<ThreadCounts, 4> countsFor(Impl impl);

} // namespace mlir::wtc

#endif // WTC_PROGRESSTABLE_H
