# Thread census

How much of a program's locking falls on mutexes that only one thread ever
takes, and how much of its `std::shared_ptr` reference counting falls on
control blocks that only one thread ever touches. Those shares are the
ceilings for removing a mutex, or making a count non-atomic, by proving the
object confined to a thread, measured before any analysis is written, on
the program as it runs.

`libwtc_threadcensus.so` is preloaded into an unmodified binary. It
interposes `pthread_mutex_lock`, `pthread_mutex_trylock` and
`pthread_mutex_unlock`, which is what `std::mutex` and `std::lock_guard`
call as well as the C API, and keeps for every mutex the set of threads
that acquired it (up to eight; more is "more than eight") and the count of
acquisitions. The re-acquisition inside `pthread_cond_wait` and
`pthread_cond_timedwait` is counted as an acquisition, since it is one;
`pthread_mutex_destroy` retires the mutex's entry (its counts go into the
totals, and into the list of the busiest if it is one of them, and its slot
is freed), so a mutex that later lives at the same address is counted apart
and a program that makes and destroys millions of objects does not fill the
table. `pthread_mutex_init` on an address that already has an entry retires
it too, for a mutex freed without being destroyed. A `std::mutex` passes
through neither: libstdc++ initialises it statically and its destructor
does nothing, so a `std::mutex` at the address of a dead one merges into
its entry, which errs towards "more than one thread". At exit it writes to
`WTC_CENSUS_OUT` (stderr when unset; a process forked from the first one,
or a program it executes with the library still preloaded, writes to
`WTC_CENSUS_OUT.<pid>`):

```
threadcensus: 2 mutexes, 2000 acquisitions (0 on mutexes the table could not hold; table of 1048576)
single-thread mutexes: 1 of 2; their acquisitions: 1000 of 2000 (50.0%)

top 2 by acquisitions (address, acquisitions, threads, first event's stack):
...
```

The call stack of each mutex's first acquisition says where it is, through
`backtrace_symbols_fd`: build the program with `-fno-omit-frame-pointer`
and, for names, with symbols (`-g` or at least not stripped; `-rdynamic`
names static functions too, and `docs/census/symbolize.py` names every
frame of a `-g` build after the run).

```sh
cmake -S tools/census -B build-census && cmake --build build-census
cd build-census && ctest --output-on-failure     # the self-tests, known answers
LD_PRELOAD=$PWD/libwtc_threadcensus.so WTC_CENSUS_OUT=/tmp/x.census ./the-program args
```

What the number is and is not: a mutex one thread took on this input may
be taken by a second on another, so the share is an upper bound for the
input it was measured on. Threads are told apart by `pthread_self`; an id
reused after a join counts as the old thread, which errs the same way. The
table holds 2^20 mutexes by default (`WTC_CENSUS_TABLE=<entries>` sets it,
rounded up to a power of two; about 190 bytes an entry, mapped at start and
touched as used); acquisitions beyond that are counted in the total and
reported, so the share stays a share of everything. Nothing is allocated on
the locking path, and the library is C, so it loads under C programs; its
symbols are resolved in a constructor, and locks taken by the dynamic
loader before that are not seen.

Not seen either: `pthread_mutex_timedlock` and `clocklock`, rwlocks,
spinlocks, and anything built on atomics.

## Reference counts

libstdc++'s counts are inline atomics, so nothing can interpose them; the
program calls into the library instead, in one of two ways, and the report
gains a second section of the same form ("single-thread control blocks: ...;
their count operations: ...").

- **`wtc-cc --census`** runs the `--wtc-census` pass, which puts a call
  `wtc_rc_note(control block, operation)` at the entry of each of
  `_Sp_counted_base<_S_atomic>`'s functions the module defines: its
  constructor (the block made, so the thread that made it is one that
  touched it), copy, release, `weak_ptr` lock, weak copy, weak release, and
  `_M_destroy`, which retires the block's entry as a destroyed mutex's is
  retired. Every operation is seen; a block moved from one `shared_ptr` to
  another is not touched by the move. The call is to a weak symbol, so the
  program must run with the library preloaded.
- **`-include tools/census/rc_hooks.h`**, for a program built by an ordinary
  compiler: the header defines libstdc++'s race-detector macros, which
  `_Sp_counted_base` invokes on every release and before destroying a block.
  Releases and destruction are seen, copies are not: the operation count is
  about half, a thread that copies a pointer it never releases is missed,
  and so is the thread that made the block, all of which err towards "one
  thread". Shares measured this way are upper bounds and nothing more. The
  other counts that use those macros (locale facets, the copy-on-write
  string) are filtered out at compile time by the enclosing function's name.

The self-tests check both with known answers (`selftest_rc.c`,
`selftest_rc_hooks.cpp`), the first also in a table of 4096 entries for
200,006 blocks; `test/Transforms/Census` and
`test/Integration/census_refcounts.cpp` check the pass, and
`test/Integration/census_preload.cpp` a `--census` program run under the
library, with a forked child.
