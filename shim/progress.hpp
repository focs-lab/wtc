#pragma once

// Promises a function makes about progress, which the compiler checks
// (--wtc-check-progress, decisions/0004).
//
// WTC_LOCK_FREE on a function promises that nothing it can reach blocks:
// no mutex, no allocator, no input or output, no call the compiler cannot
// see into. A violation fails the build and names the call chain that
// reaches the blocking operation. It is a necessary condition for
// lock-freedom, not a proof of it, though a loop that spins waiting for
// another thread, or for an element of a queue, fails it too.
//
// WTC_WAIT_FREE promises that, and that every path is wait-free: no retry
// on a compare-and-swap another thread can make fail, no operation that is
// only lock-free under the thread counts the compiler proves, no wait for
// data. WTC_WAIT_FREE_WITHIN(k) also promises that the worst case is at
// most k steps (operations on wtc objects by their bounds, and the
// function's own atomic accesses), a number the compiler must be able to
// compute from the counts it proves. WTC_PROGRESS_CRITICAL marks a
// latency-critical path: wait-free with a worst case that is a polynomial
// in the counts (not only an amortised bound), and no effect on first use
// (a thread-local registration, a static-initialisation guard).
//
// WTC_BATCH on a function allows the compiler to batch the pushes of its
// loops (--wtc-batch-pushes): elements are then published every WTC_BATCH_K
// pushes and when the loop ends, rather than one at a time, so a consumer
// sees each one later. Only loops in which nothing could be waiting for an
// unpublished element are touched. It also allows the compiler to hold a
// mutex across the critical sections of a loop (--wtc-batch-locks),
// letting it go every WTC_LOCK_BATCH_K sections and when the loop ends, so
// another thread waits longer for it. Only loops in which nothing could
// wait for another thread are touched.
//
// Without the WTC passes the attributes do nothing.
#define WTC_LOCK_FREE [[clang::annotate("wtc_progress_lock_free")]]
#define WTC_WAIT_FREE [[clang::annotate("wtc_progress_wait_free")]]
// ClangIR does not carry an annotation's arguments: the bound is part of
// the name. k must be a decimal literal.
#define WTC_WAIT_FREE_WITHIN(k) [[clang::annotate("wtc_progress_wait_free_within_" #k)]]
#define WTC_PROGRESS_CRITICAL [[clang::annotate("wtc_progress_critical")]]
#define WTC_BATCH [[clang::annotate("wtc_batch")]]
