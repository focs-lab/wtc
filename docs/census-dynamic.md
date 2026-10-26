# Which mutexes and reference counts one thread touches: a census

The question behind the audit's thesis B (strength reduction on `std::mutex`,
`pthread_mutex_t` and `std::shared_ptr`): how much of a program's
synchronisation falls on objects that only one thread ever touches. That
share is the ceiling for removing a mutex, or making a reference count
non-atomic, by proving the object confined to a thread. It is measured here
on the programs as they run, before any analysis is written.

The tool is `tools/census`, a preloaded library that counts, for every
mutex, the threads that acquire it and how often (the re-acquisition after
a condition-variable wait included), and, for every `std::shared_ptr`
control block, the threads that make, copy, release or lock it. Reference
counts are seen in two ways: a build through `wtc-cc --census` reports every
operation, and an ordinary build with `tools/census/rc_hooks.h` included
reports releases and destruction only (`tools/census/README.md`).
`docs/census/` holds every report as `run.sh` leaves it, the drivers and the
scripts that produced them.

Measured 26 Oct 2026 on one socket of a two-socket host, untimed, inside the
group's container; one run per program and load, seconds to a minute each.
The census of 23 Oct measured one load per program; this one repeats it and
adds a second load for each, RocksDB, and the reference counts.

## Mutexes

| program | load | acquisitions | on single-thread mutexes |
|---|---|---|---|
| memcached (corpus clone, 2d51e36) | 4 workers, 4 clients, 50 k set/get each | 1,780,928 | **64.9%** (1,539 of 2,840 mutexes) |
| | 8 workers, 8 clients, 100 k each | 7,697,687 | **65.1%** (2,901 of 5,404) |
| | the same, one client asking for `stats` every 1,000 requests | 7,639,184 | 10.5% (2,886 of 5,404) |
| Redis 7.2.4 | `--io-threads 4`; `redis-benchmark -t set,get -n 200000 -c 50 --threads 4` | 711 | 14.8% (5 of 8) |
| | `--io-threads 8`; five commands, 1 M requests each, `-c 100 --threads 8` | 5,102 | 9.9% (5 of 12) |
| SQLite 3.45.1 (amalgamation) | 4 threads on one serialized connection to an in-memory database, 5 k inserts and 5 k selects each | 420,988 | 0.0% (2 of 5) |
| | 4 threads, a connection each, to one file in WAL mode | 428,402 | **37.4%** (6 of 13) |
| spdlog 1.14.1 as released | 4 logging threads, 300 k messages each, 1 worker, null sink | 3,315,635 | 0.0% (3 of 4) |
| | the same, 2 workers | 2,499,291 | 0.0% (3 of 4) |
| spdlog, the compiler's build | either load | 7 | 100% (3 of 3) |
| PARSEC dedup, native input | 4 threads per stage | 1,009,780 | 4.6% (37,847 of 248,928) |
| | 8 threads per stage | 1,007,019 | 3.7% (33,785 of 248,932) |
| PARSEC dedup, simdev input | 4 threads per stage | 1,004 | 28.1% (282 of 568) |
| RocksDB (corpus clone), `db_bench` | `fillrandom`, 4 threads, 500 k writes each | 2,021,458 | 0.0% (151 of 192) |
| | `fillrandom`, 8 threads | 2,194,778 | 0.0% (152 of 201) |
| | `readrandom`, 4 threads, 500 k reads each | 1,145 | **38.2%** (125 of 144) |
| | `readrandom`, 8 threads | 1,203 | 36.8% (125 of 145) |

The shares move little between runs (three runs of everything: memcached
63.8–65.4%, SQLite per connection 37.4–37.8%, dedup 3.7–4.7%). Some counts
move a great deal, because they count waits rather than work: Redis's I/O
threads park and wake on their mutexes (711 to 1,074 acquisitions under the
first load, 5,102 to 320,730 under the second), and RocksDB's write group
waits on its writers' (2.1 to 6.8 million acquisitions at eight threads).

**The verdict.** The criterion set for thesis B on 23 Oct was at least 30%
of acquisitions on single-thread mutexes in three of five programs. Six
programs are measured here; taking each one's best load, three reach 30%:
memcached (65.1%), SQLite with a connection per thread (37.4%) and RocksDB's
reads (38.2%). None of the three is a share a compiler would win:

- **memcached.** The 65% is the per-worker statistics locks (below). A
  load that asks for `stats` brings it to 10.5%: the aggregation that makes
  them shared is in the program and runs on another thread, so no proof of
  confinement exists for them.
- **SQLite.** The 37.4% is the per-connection mutexes when each thread opens
  its own connection. A proof is possible here: each connection is opened
  and used inside one thread's function. But SQLite already lets the
  application say so, per connection (`SQLITE_OPEN_NOMUTEX`) or per build
  (`SQLITE_THREADSAFE=2`), so a compiler that proved it would save what a
  flag already saves. The two SQLite loads differ in two things, the
  database (in memory, or a file in WAL mode) and the sharing (one
  connection, or one per thread); the share comes from the second.
- **RocksDB.** The 38% under `readrandom` is a share of about 1,200
  acquisitions for two to four million reads: the read path takes no mutex.
  The write path's millions of acquisitions are on the database mutex and
  the write group's, all shared.

So the criterion is met on paper and not in substance, and thesis B stays a
second paper, as decided on 23 Oct. Where the single-thread share is large
it depends on the load (memcached) or on a choice the library already
exposes (SQLite).

### memcached: the per-thread statistics locks

The busiest mutexes are each worker's `thread->stats.mutex`, first locked in
`try_read_network` (`memcached.c:2474`) from `worker_libevent`, taken by the
worker on every read to add to its own counters: 500,001 acquisitions each
under the 8-worker load, one thread each. With the `stats` client the same
mutexes are taken 500,101 times by two threads: the 101 `stats` requests
aggregate every worker's counters under those locks, on the thread serving
that client. What would remove the cost is not a proof of confinement but a
design in which the owner writes its counters without a lock and the
aggregator reads them with one. The rest are the item and LRU locks (eight
threads each) and the slab allocator's, shared as they should be.

### Redis: almost no mutexes on the request path

The busiest acquisitions are the `io_threads_mutex` entries the main thread
uses to park and release the I/O threads, two threads each. Redis's I/O
threads otherwise coordinate through atomics and per-thread job lists, which
the census does not see.

### SQLite: shared or not is the application's choice

On one serialized connection every statement from every thread goes through
the connection's mutex and every allocation through the memory subsystem's.
With a connection per thread, each connection's mutex is taken about 40,000
times by its own thread only (`sqlite3_mutex_enter` from `work`,
`sqlite_driver.c:38`). The shared mutexes left are the write-ahead log's
shared-memory node (about 160,000 acquisitions, first in `sqlite3OsShmMap`)
and the unix file system layer's global mutex (`staticMutexes`, VFS1: about
100,000), both taken by all four threads, as they must be for connections
to one file.

### spdlog: the queue's mutex, and nothing after the port

As released, nearly every acquisition (2.5 to 3.3 million) is on the pool
queue's mutex, taken by the logging threads and the worker, and by both
again after condition-variable waits: the mutex the port replaces with
`wtc::queue`. The compiler's build takes a mutex seven times, at the
registry's initialisation and shutdown.

### dedup and RocksDB

dedup locks one mutex per bucket of its deduplication table, taken by
whichever stage thread reaches the bucket; the 4–5% single-thread share is
the buckets only one thread happened to reach, and the simdev input's 28.1%
says only that a small input touches few buckets. Its stage queues' mutexes
are the ones the compiler replaces. RocksDB's writes go through the database
mutex (`InstrumentedMutex`) and the write path's (`DBImpl::PreprocessWrite`),
about a million acquisitions each, taken by every writer; its reads take
almost none.

### Addresses reused without being destroyed

A mutex freed without `pthread_mutex_destroy`, and another initialised at
its address, would merge into one entry and look shared. The census retires
the old entry on `pthread_mutex_init` as on `pthread_mutex_destroy`. The
first of the three runs was made without that; the number of mutexes each
program reports is the same in all three for memcached, Redis, SQLite,
dedup and RocksDB's writes, and within two for RocksDB's reads, so no share
here was inflated by reuse. A `std::mutex` passes through neither call:
libstdc++ initialises it statically and its destructor does nothing, so a
`std::mutex` living where a dead one lived still merges with it, which errs
towards "more than one thread", against the share. spdlog keeps its mutexes
for the program's life, and RocksDB's `port::Mutex` calls
`pthread_mutex_init`. The 23 Oct report of dedup counted 266,675 mutexes
and these 248,928 with the same input; retirement on initialisation does
not explain the difference, which was not chased further.

## Reference counts

Two of the six programs use `std::shared_ptr` on their paths at all: spdlog,
measured with every operation, and RocksDB, measured with releases only.
That is too little to judge the 23 Oct criterion for reference counts (at
least 20% of count operations on single-thread objects in most programs),
and this instrument cannot judge it either way. What it shows:

| program | build | count operations | on single-thread control blocks |
|---|---|---|---|
| spdlog as released, 1 or 2 workers | `wtc-cc --census`, every operation | 9,616,424 | 25.0% (1,200,005 of 2,408,202 blocks) |
| | `rc_hooks.h`, releases only | 4,808,210 | 50.1% (2,408,199 of 2,408,202) |
| spdlog, the port's general build and the compiler's | `wtc-cc --census` | 9,600,038 | 25.0% (1,200,005 of 2,400,009) |
| RocksDB `fillrandom`, 4 / 8 threads | `rc_hooks.h`, releases only | 3,703 / 5,579 | 31.6% / 21.5% |
| RocksDB `readrandom`, 4 / 8 threads | `rc_hooks.h`, releases only | 1,628 / 1,654 | 75.4% / 74.6% |

In spdlog half of the count operations are on two control blocks, the
logger's and the thread pool's, each locked from a `weak_ptr` and released
once per message (2.4 million operations each, by five and six threads).
The other half is 2.4 million control blocks, each made and released once:
the shared state of the `std::promise<void>` that every `async_msg` carries
(`details/thread_pool.h:31`), about two per message. Half of them are made
and released on one thread and half on two. In between they are moved, and
a move touches no count, so the census cannot tell whether a block crossed
threads and came back. The 25% is therefore an upper bound on one program,
over the cheapest operations there are (libstdc++ 12 and later do not
decrement a sole reference atomically). Without the making counted, as
under `rc_hooks.h`, every such block looks single-thread and the share
doubles to 50%: the instrument's bias, not the program's.

RocksDB keeps `std::shared_ptr` off its hot paths: a few thousand releases
for millions of operations, and `rc_hooks.h` sees neither copies nor the
making, so its shares lean towards "one thread". Its own reference counts
(the super-version's and the column family's) are plain atomics, which the
census does not see.

## What the census cannot see

Atomics and anything built on them: spinlocks, Redis's I/O coordination,
RocksDB's own reference counts. Rwlocks, `pthread_mutex_timedlock` and
`clocklock`, and `pthread_spin_lock` are not interposed; locks taken before
the library's constructor has resolved its symbols are not counted. A
`shared_ptr` moved from thread to thread is not touched by the move. A
destroyed or re-initialised object's entry is retired into the totals, so a
later object at the same address is counted apart (except a `std::mutex`,
above). Threads are told apart by `pthread_self`, so an id reused after a
join counts as the old thread. And an object one thread touched on this
load may be touched by another on the next: memcached's `stats` is the
example. Every share is an upper bound for its load.

MySQL, the seventh program planned, was not run: its server needs a build of
an hour or more and a load driver (sysbench) that is not in the container.

## Reproducing

```
docs/census/build.sh <src-dir> <out-dir>         # library, memcached, Redis, SQLite
docs/census/build_rc.sh <spdlog-dir> <rc-dir>    # spdlog for the counts
docs/census/build_rocksdb.sh <src-dir> <db-dir>  # db_bench with rc_hooks.h
docs/census/run.sh <out-dir> <spdlog-bin-dir> <dedup-bin-dir> <dedup-inputs> \
                   <db-dir>/rocksdb-build/db_bench <rc-dir>/bin
```

`<src-dir>` holds the corpus clones of memcached and RocksDB, `redis-7.2.4`
(GitHub release tarball), `sqlite-amalgamation-3450100` (sqlite.org) and
`gflags-2.2.2`; spdlog's binaries are those of
`benchmarks/apps/spdlog/build.sh`, dedup's those of
`benchmarks/apps/dedup/build.sh`, and `<dedup-inputs>` holds PARSEC's
`native/` and `simdev/` inputs. The programs are built with
`-O2 -g -fno-omit-frame-pointer` so that the reports name the first event's
stack. `run.sh` ends with `docs/census/symbolize.py`, which names the
frames with `addr2line`, names each binary by file name only and cuts
frames at 160 characters. Redis is built with `MALLOC=libc`, so that the
census sees Redis and not its allocator.
