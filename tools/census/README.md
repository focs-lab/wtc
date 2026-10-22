# Thread census

How much of a program's locking falls on mutexes that only one thread ever
takes. That share is the ceiling for removing a mutex by proving it confined
to a thread, measured before any analysis is written, on the program as it
runs.

`libwtc_threadcensus.so` is preloaded into an unmodified binary. It
interposes `pthread_mutex_lock`, `pthread_mutex_trylock` and
`pthread_mutex_unlock`, which is what `std::mutex` and `std::lock_guard`
call as well as the C API, and keeps for every mutex the set of threads
that acquired it (up to eight; more is "more than eight") and the count of
acquisitions. At exit it writes to `WTC_CENSUS_OUT` (stderr when unset):

```
threadcensus: 2 mutexes, 2000 acquisitions (0 on mutexes the table could not hold)
single-thread mutexes: 1 of 2; their acquisitions: 1000 of 2000 (50.0%)

top 2 by acquisitions (address, acquisitions, threads, first acquisition's stack):
...
```

The call stack of each mutex's first acquisition says where it is, through
`backtrace_symbols_fd`: build the program with `-fno-omit-frame-pointer`
and, for names, with symbols (`-g` or at least not stripped; `-rdynamic`
names static functions too).

```sh
cmake -S tools/census -B build-census && cmake --build build-census
cd build-census && ctest --output-on-failure     # the self-test, known answer
LD_PRELOAD=$PWD/libwtc_threadcensus.so WTC_CENSUS_OUT=/tmp/x.census ./the-program args
```

What the number is and is not: a mutex one thread took on this input may
be taken by a second on another, so the share is an upper bound for the
input it was measured on. Threads are told apart by `pthread_self`; an id
reused after a join counts as the old thread, which errs the same way. The
table holds 65536 mutexes; acquisitions beyond that are counted in the
total and reported, so the share stays a share of everything. Nothing is
allocated on the locking path, and the library is C, so it loads under C
programs; locks taken by the dynamic loader before the library's own
symbols resolve are not seen.

Reference counts (`std::shared_ptr`) cannot be counted this way, since
their atomics are inlined; that census is a pass of `wtc-opt`.
