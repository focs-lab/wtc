#pragma once

// Promises a function makes about progress, which the compiler checks
// (--wtc-check-progress, decisions/0004).
//
// WTC_LOCK_FREE on a function promises that nothing it can reach blocks:
// no mutex, no allocator, no input or output, no call the compiler cannot
// see into. A violation fails the build and names the call chain that
// reaches the blocking operation. It is a necessary condition for
// lock-freedom, not a proof of it: a loop that spins waiting for another
// thread is not judged.
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
#define WTC_BATCH [[clang::annotate("wtc_batch")]]
