# Which mutexes one thread takes: a census of five programs

The question behind the audit's thesis B (strength reduction on `std::mutex`
and `pthread_mutex_t`): how much of a program's locking falls on mutexes
that only one thread ever takes. That share is the ceiling for removing a
mutex by proving it confined to a thread, measured here on the program as
it runs, before any analysis is written. The tool is `tools/census`, a
preloaded library that counts, for every mutex, the threads that acquire
it and how often, the re-acquisition after a condition-variable wait
included; `docs/census/` holds the top of each report, the drivers
(`sqlite_driver.c`, `memcached_load.py`) and the scripts (`build.sh`,
`run.sh`) that produced them. Measured 23 Oct 2026 on one socket of a
two-socket host, untimed, inside the group's container; one run each, a
few seconds of load.

| program | load | threads locking | acquisitions | on single-thread mutexes |
|---|---|---|---|---|
| memcached (corpus clone, 2d51e36) | 4 workers, 4 clients, 50 k set/get each | 9 | 1,808,264 | **64.3%** (1,539 of 2,840 mutexes) |
| Redis 7.2.4 | `--io-threads 4`, `redis-benchmark -t set,get -n 200000 -c 50 --threads 4` | 6 | 1,572 | 6.7% (5 of 8) |
| SQLite 3.45.1 (amalgamation) | 4 threads on one serialized connection, 5 k inserts and 5 k selects each | 5 | 422,808 | 0.0% (2 of 5) |
| spdlog 1.14.1 as released | 4 logging threads, 300 k messages each, 1 worker, null sink | 6 | 3,301,145 | 0.0% (3 of 4) |
| spdlog, the compiler's build | the same | 1 | 7 | 100% (3 of 3) |
| PARSEC dedup, native input | 4 threads per stage | > 8 | 1,010,837 | 4.6% (37,952 of 266,675) |
| PARSEC dedup, simdev input | 4 threads per stage | 8 | 964 | 29.3% (282 of 568) |

The criterion the audit set for thesis B, at least 30% of acquisitions on
single-thread mutexes in three of five programs, is met by one: memcached.
What the one is made of matters more than the number.

## memcached: the per-thread statistics locks

The four mutexes that carry 1,000,004 of the 1,163,201 single-thread
acquisitions are first locked in `try_read_network` (`memcached.c:2475`),
one per worker, from `worker_libevent`: each worker's `thread->stats.mutex`,
taken by the worker itself on every read from a connection to add to its
own counters. A second thread takes them only when a client asks for
`stats`, which aggregates every worker's counters under the same locks;
this load never did. So the share is real for this input and not provable
statically: the aggregation path exists and runs on another thread. What
would remove the cost is not a proof of confinement but a lock whose
uncontended path is cheap (the glibc mutex's is a lock-prefixed exchange
on each side) or a redesign where the owner writes its counters without a
lock and the aggregator reads them with one. The remaining 35% are the
item and LRU locks (first locked from the slab-mover thread, 8 threads on
each) and the slab allocator's, shared as they should be.

| mutex (first lock) | acquisitions | threads |
|---|---|---|
| worker's `thread->stats.mutex`, `try_read_network` | 250,001 × 4 | 1 each |
| LRU lock, `fill_item_stats_automove` (`items.c:708`) | 95,688 | 8 |
| LRU lock, `fill_item_stats_automove` (`items.c:702`) | 76,088 | 8 |

## Redis: almost no mutexes at all

1,572 acquisitions for 200,000 requests. Redis's I/O threads coordinate
through atomics and per-thread job lists; its mutexes are the three
`io_threads_mutex` entries the main thread uses to park and release the
I/O threads (489 acquisitions each, 2 threads) and the asynchronous
free-list lock in `freeClientAsync` (101, one thread). Nothing the census
sees is on the request path; what is there, the census cannot see
(`__atomic` operations and spinning on counters).

## SQLite: a shared connection serialises everything

With one connection in serialized mode, every statement from every thread
goes through the connection's mutex, and every allocation through the
allocator's: 261,151 acquisitions on the mutex first locked in
`mallocWithAlarm` (the memory subsystem's), 160,048 on the connection's,
1,578 on the page cache's, all by all four threads; the two single-thread
mutexes are taken 31 times in all at initialisation. A program that opens one connection per thread would have
every connection mutex confined to its thread, and SQLite already offers
to compile that locking out (`SQLITE_THREADSAFE=2`): the confinement is
decided by configuration, not discoverable from the code.

## spdlog: the queue's mutex, and nothing after the port

As released, 3,301,138 of the 3,301,145 acquisitions are on the pool queue's
mutex (`mpmc_blocking_queue::dequeue`, first locked by the worker), taken by
the four logging threads, the worker and the main thread's flush, 1.2
million of them the worker and the blocked producers taking it back after
a condition-variable wait: exactly the mutex the port replaces with
`wtc::queue`. The compiler's build takes a
mutex 7 times, all at the registry's initialisation and shutdown, none per
message; the null sink is lock-free in both builds (`null_sink_mt` uses
`null_mutex`). The remaining per-message synchronisation in the port is
not a mutex: the logger's `shared_ptr` count, incremented by the logging
thread and decremented by the worker, which the census cannot count.

## dedup: locks per hash bucket, taken by whoever gets there

On the native input dedup takes 266,675 distinct mutexes: one per hash
bucket of its deduplication table, locked by whichever stage thread
reaches the bucket, and the stage queues' (198,093 acquisitions each on
the two between Compress and Deduplicate, 2 and 4 threads, first locked in
`sub_Compress` and `sub_Deduplicate`; 47 k to 17 k on the others, 8 and
more threads). The 4.6% on single-thread mutexes are the 37,952 buckets
only one thread happened to reach on this input; on the small simdev
input the same share is 29.3% of 964 acquisitions, which says only that a
small input touches few buckets. The queues' mutexes (`queue_dequeue`) are
the ones the compiler would replace.

## What the census cannot see

Atomics, spinlocks built on them, and `std::shared_ptr`'s reference
counts, whose operations are inlined; Redis and the spdlog port
synchronise mostly that way. `pthread_mutex_timedlock` and `clocklock`,
rwlocks and `pthread_spin_lock` are not interposed. The re-acquisition
after `pthread_cond_wait` and `pthread_cond_timedwait` is counted; locks
taken before the library's constructor has resolved its symbols are not.
A destroyed mutex's entry is closed, so a later mutex at the same address
is counted apart. Threads are told apart by `pthread_self`, so an id
reused after a join counts as the old thread. And a mutex one thread took
on this input may be taken by another on the next: memcached's `stats`
command is the example. The shares are upper bounds for the loads above.

## Reproducing

```
docs/census/build.sh <src-dir> <out-dir>     # census library, memcached, Redis, SQLite driver
docs/census/run.sh <out-dir> <spdlog-bin-dir> <dedup-bin-dir> <dedup-input>
```

`<src-dir>` holds the corpus clone of memcached, `redis-7.2.4` (GitHub
release tarball) and `sqlite-amalgamation-3450100` (sqlite.org); spdlog's
binaries are those of `benchmarks/apps/spdlog/build.sh`, dedup's those of
`benchmarks/apps/dedup/build.sh`. The programs are built with
`-O2 -g -fno-omit-frame-pointer` so that the reports name the first lock's
stack; Redis with `MALLOC=libc`, so that the census sees Redis and not its
allocator.
