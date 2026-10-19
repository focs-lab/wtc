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
// Without the WTC passes the attribute does nothing.
#define WTC_LOCK_FREE [[clang::annotate("wtc_progress_lock_free")]]
