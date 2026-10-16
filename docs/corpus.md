# Status, 16 October 2026: recounted, and no reported pair survives review

This file was written in September, audited, and found wrong in places; the
fixes were audited again on 16 October and found wrong in more. This section
says where it stands now. The sections below keep what they said, with
corrections marked where they were made, because a retraction that deletes
what it retracts teaches nothing.

**The result of this phase is negative, and narrower than it first looked.**
On amalgamated memcached the analysis now reports 155 definite and 503
possible pairs inside objects. Every object was read against the source,
and no pair describes traffic that separating fields would remove. Two
measurements under load agree: applying the verdict moves throughput by
less than 1%, and its 95% intervals lie between -3.2% and +2.0%, without
and with eviction. What the analysis cannot see at all --
arrays indexed per class or per thread, and heap objects -- was not
examined, so this is not a claim that memcached has nothing to fix.

## What was wrong, and what it is now

| what | was | is | why it moved |
|---|---:|---:|---|
| source files scanned | 112,831 | 110,016 | a checked-in `cmake-build-release` was being scanned as source |
| sites | 1,572 | **1,659** | build-tree files, preprocessor lines and comments were counted; seven idioms were missed (below) |
| two totals in one document | 1,572 and 1,531 | **1,659 and 1,659** | the census counted directives, the classifier dropped them; both now set aside every line that starts with `#` |
| LLVM sites | 34 | **0** | all 34 were lit-test output in the build tree |
| PARSEC sites | 6 | **21** | the padding filter was case-sensitive. 16 of the 21 are in PARSEC's vendored copy of TBB, so they repeat oneTBB's idiom rather than add a codebase's own decisions |
| oneTBB sites | 1 | **25** | `alignas(max_nfs_size)`, the library's own idiom, was not searched for |
| Linux sites | 773 | **836** | `____cacheline_internodealigned_in_smp` (47), `__cacheline_group_begin_aligned` (25), `__aligned(SMP_CACHE_BYTES)` (14) and `CACHELINE_PADDING` (10) were not searched for; comment lines were counted |
| fields / objects | 675 / 856 (44% / 56%) | **761 / 898 (46% / 54%)** | the nesting tracker could not see a brace on its own line, nor skip comments and string literals |
| field annotations on one member | 648 of 675 (96%) | **687 of 761 (90%)** | two of the three ways the kernel writes a *group* were missed |
| files with several field annotations | 300, at most 24 | **153, at most 17** | recounted from the corrected classification |
| memcached pairs, amalgamated | 397 definite, 546 possible | **155 definite, 503 possible, 349 cold** | the walk had silently stopped at 10,000 function instances (it needs 132,997), and it never saw the worker pool |
| memcached pairs, per file | 20 definite, 422 cold | **0 definite, 460 possible** | a thread the module cannot account for no longer gives a definite or a cold verdict |
| globals `--wtc-align-by-owner` picks | 73, 4,254 B | **60, 3,489 B** | the same two defects; the comparison point, 141 mutable globals and 7,890 B, holds |

The idioms added are the four kernel ones above, DPDK's `RTE_CACHE_GUARD`
(10), oneTBB's `alignas(max_nfs_size)` (18), and project macros that align
to a line -- rocksdb's `ALIGN_AS(CACHE_LINE_SIZE)`, jemalloc's
`JEMALLOC_ALIGNED(CACHELINE)`, abseil's `ABSL_CACHELINE_ALIGNED`, folly's
`cacheline_aligned<T>` (31 together). The plain `__cacheline_group_begin`
is *not* counted: it is a zero-length marker that aligns nothing. A sample
read during the second audit put the census's precision near 75% -- a
libuv control-message buffer and an MMIO window among the hits -- which is
a property of matching text, and the reason the census is an oracle to
compare against rather than ground truth.

Two of the corrections reverse conclusions drawn below and are flagged
there: people do group fields as well as mark single boundaries (10%, not
4%), and the memcached counts that the density and alignment sections stand
on came from a walk that had stopped partway and had not seen the workers.

## What the analysis got wrong, and the tests that now hold it

Each of these changed verdicts; each has a test that fails on the code it
fixed and passes now. Two of the defects were introduced by the first round
of fixes and found by the second review.

- **The worker pool was invisible.** A spawn's entry was resolved only
  when it named a function directly. memcached starts its workers through
  `create_worker(worker_libevent, ...)`, whose `pthread_create` names a
  parameter, so the workers became "functions whose address is taken" --
  any thread, any number of times -- and every pair they take part in was
  at best possible. The entry is now resolved through the call site that
  passed it, and a function whose address flows only into such entries is
  not also a thread of its own -- under the same notion of a single
  assignment the resolution uses, or a thread can vanish from the walk.
  (`spawn_through_parameter.c`, `entry_assigned_in_a_branch.c`)
- The start-up phase was a property of a function instance, so a function
  that spawns a thread was "start-up" all the way through, after the spawn
  too. (`startup-ends-at-the-spawn.mlir`) And it ended only at a spawn the
  module could see, which std::thread's is not: `_M_start_thread` is now
  recognised by name (`std_thread_main.cpp`). A spawn from a global
  constructor ends it before main begins, and for every constructor too,
  since their order is not the source's (`constructor-spawn-ends-startup.mlir`,
  `constructor_after_spawn.c`).
- `cold` -- "this pair costs nothing" -- was computed from all threads,
  including ones the module cannot account for, and ran before `possible`,
  so an unknown pair could be printed as a resolved one.
  (`cold-is-a-claim.mlir`, `unknown-is-not-cold.mlir`)
- `possible` was computed from whether a field had *ever* been written, so
  memcached's option parsing, which runs once before any thread exists,
  paired with every command that rewrites a setting.
  (`possible-needs-traffic.mlir`)
- Which functions reach a field access, and which can spawn, was answered
  by a recursive search that cached "no" while a cycle was still being
  explored; inside recursion the answer depended on where the search
  entered. Both are now fixpoints over the call graph.
  (`recursion-does-not-hide-a-pair.mlir`)
- A thread spawned from a placeholder for an unknown caller was treated as
  accounted for, and a function whose address is taken was given its own
  accounted-for thread, so two callbacks of one single-threaded event loop
  came out as a definite pair. (`spawn-from-a-placeholder.mlir`,
  `callbacks-are-not-threads.mlir`)
- A stack slot never gave a definite verdict, even in `main`, which runs
  once. (`stack-slot-in-main.mlir`)
- **Every object was assumed to start on a line boundary.** Unless it is
  aligned to a line, where it starts is the linker's choice, so two fields
  56 bytes apart may or may not share a line, and two adjacent fields may
  straddle one. Each pair is now reported with the share of the possible
  starting positions in which it shares a line. (`placement.c`)
- `--wtc-align-by-owner` remembered one partner per global, so its refusal
  and its remark were about whichever pair the walk met first.
  (`partner-cannot-be-raised.mlir`, `every-partner-counts.mlir`)
- No pass checked whether the walk had hit its instance cap. On memcached it
  had, thirteen-fold. Every pass now refuses past the cap, which is 250,000
  and an option. (`instance-cap.mlir`)
- A record-padding pass, written after the September measurements and
  never committed, is not kept. Layout cannot be changed after the front
  end: `sizeof` arrives already folded into a constant, so
  `malloc(sizeof(struct S))` keeps the old size while the struct grows,
  which is a heap overflow with no remaining evidence to refuse on. Layout
  changes belong in the source, before the front end runs.

Several tests that passed whatever the pass did now check a summary line
the pass always prints, and counts are anchored so that `0 definite` cannot
match `10 definite`. One test compiles a program, transforms it, links,
runs it, and asks the running program where its two counters landed
(`align_by_owner_runs.c`); before it, no test executed anything this
project had changed.

## The review: every memcached object, read against the source

A pair survives only if separating the two fields would remove cache-line
transfers the program actually performs at a rate that matters. Three
questions decide it.

1. **Who writes each field?** If the same threads write both, each field's
   line travels between them anyway, and separation removes nothing.
2. **Do the two sides run at the same time?** A hand-off -- one side writes,
   signals, and the other reads after -- moves the line once per hand-off.
3. **How often does each side run?** That is the question the analysis
   cannot answer, and the one every pair below is decided by.

Whether a common lock serialises the accesses does not decide it alone,
and the retraction below is corrected on that point: a lock and the data it
guards sit on different lines, so a critical section fetches the data line
separately, and separation spares that fetch when the writers are disjoint.

| object | definite | possible | verdict |
|---|---:|---:|---|
| `@stats_state` (56 B) | 49 | 17 | **no.** Every write but two is under `STATS_LOCK`; the two are `reserved_fds`, written once per worker while the workers are set up, and `accepting_conns`. The per-request writes are to `curr_items` and `curr_bytes`, by the workers on every link and unlink and, under eviction, by the LRU maintainer, the crawler, the slab mover and the extstore writer too -- all to the *same* two fields, which is true sharing that no layout removes. The fields on the other side of each pair are written per hash-table expansion, per crawl, per slab move, per connection, or once per worker at start. |
| `@stats` (224 B) | 78 | 89 | **no.** Every write is under `STATS_LOCK` (all 31 sites checked). The per-request write, `total_items`, is the same field for every thread that links an item. The fastest writer of any other field is the LRU maintainer's `lru_maintainer_juggles`: under eviction it does not sleep and was measured at about 8,000 a second, against about 230,000 sets a second from the workers -- one transfer of the line in thirty. The rest are slower: the logger's flush, the slab mover, the crawler, extstore compaction, failure paths. |
| `@slab_rebal` (72 B) | 25 | 0 | **no.** A hand-off: `slabs_reassign` only try-locks `slabs_rebalance_lock`, which the rebalancer thread holds except while it waits, and the rebalancer reads `s_clsid`/`d_clsid` only after being signalled. |
| `@active_crawler_mod` (56 B) | 3 | 2 | **no.** The same hand-off under `lru_crawler_lock`, once per crawl command. |
| `@settings` | 0 | 395 | **no.** Read on every request; written by `SIGHUP`, by administrative commands (`cache_memlimit`, `verbosity`, `slabs automove`, `lru` tuning, `stats detail`), by `flush_all`, a client command that sets `oldest_live`, and by the LRU maintainer's extstore automove, which writes `ext_global_pool_min` once per window from a file the amalgamation leaves out. A workload issuing `flush_all` continuously would make one pair here matter; none that measures a cache does. |

So decision point 1 of the plan is reached for the pairs the analysis
reports: none survives. It is not reached for memcached as a whole, and
two things the review found are outside what the analysis reports:

- **One pair across objects.** `expanding` (`assoc.c:47`), read by every
  hash lookup, sits on the same line as `stats_state`, written on every
  item link: 0x455b8 against 0x45580 in the binary. That is false sharing
  between two objects, and `--wtc-align-by-owner` does separate them. It is
  the one candidate both measurements below can see.
- **What the analysis does not model.** `lru_locks[]` holds 40-byte mutexes
  aligned to 16, so neighbouring slab classes share lines; `heads[]`,
  `tails[]` and `itemstats[]` are indexed per class and written under
  different locks; the item locks are a heap array; and the summary line
  admits 4,493 field accesses on objects it could not identify. None of
  these was examined, because nothing here reports them.

## The measurements that test the review

Four builds of the same amalgamated memcached 1.6.29, each passing
memcached's own test suite (about 180,000 checks, run on a build of each arm
with `MEMCACHED_DEBUG`, which the suite needs; the one failure in every arm
is `t/line-lengths.t`, which reads a documentation file the copied tree does
not include):

- `orig`: `clang -O2`, as shipped.
- `hand`: every field in a definite pair given `__attribute__((aligned(64)))`
  -- the analysis' verdict applied as literally as possible, 50 fields in
  four objects. It was built from the verdict before the worker pool was
  visible; the later verdict adds two fields of `@stats` and one of
  `@slab_rebal`. This is maximal
  separation, not the least the verdicts need, and it moves `expanding` off
  `stats_state`'s line as a side effect, since `stats_state` grows to 704
  bytes.
- `cir`: the same source through ClangIR with no transformation: what the
  round trip itself costs.
- `ciralign`: through ClangIR with `--wtc-align-by-owner` (57 globals raised;
  the fixed pass would raise 60, the difference being three logger
  globals).

Fixed in advance: the review is overturned if `hand ÷ orig` is at least
1.02 with a 95% interval above 1.00 on any workload; the round trip is
acceptable if `cir ÷ orig` is within 2%. memcached ran four workers on four
cores of one NUMA node of a dedicated 80-thread Xeon Gold 6230 host, and
memtier eight client threads on eight other cores of the same node, inside
one container; repetitions interleaved, ten per arm, 20 s each. The governor
was `powersave` with turbo on -- fixing either needs root -- which widens the
intervals but cannot favour an arm, since every arm ran in every position.

Two workloads per leg, each with set:get in two ratios. Leg 1 is memcached
as usually benchmarked: 2 GiB of memory and uniformly random keys, so
nothing is evicted. memtier gives every client the same random sequence,
so these runs touched a small set of keys over and over, which is a
property of the tool and the same for every arm. Leg 2 answers the
objection that leg 1 leaves the background threads idle: 64 MiB of memory,
a separate key range per client, values of 32 to 4,096 bytes, and about 9
million evictions per 20-second write-heavy run, with the LRU maintainer
busy throughout. The slab mover stayed idle even so: a uniform spread of
sizes keeps the classes balanced, so its pairs, which are hand-offs
anyway, were not driven.

Throughput relative to `orig`, median of ten, with a 95% bootstrap interval:

| | leg 1, 1:10 | leg 1, 1:1 | leg 2, 1:10 | leg 2, 1:1 |
|---|---|---|---|---|
| `orig`, operations per second | 1,227,592 | 1,388,929 | 1,126,722 | 910,297 |
| `hand ÷ orig` | 1.000 [0.989, 1.010] | 1.007 [0.994, 1.016] | 0.997 [0.969, 1.020] | 0.998 [0.981, 1.017] |
| `cir ÷ orig` | 0.974 [0.956, 0.987] | 0.987 [0.968, 0.993] | 0.974 [0.963, 0.993] | 0.966 [0.941, 0.990] |
| `ciralign ÷ cir` | 0.995 [0.979, 1.014] | 1.005 [0.996, 1.020] | 0.996 [0.981, 1.009] | 1.009 [0.967, 1.036] |

**The review stands.** Applying the analysis' verdict -- every field it
names put on a line of its own, and `expanding` moved off `stats_state`'s
line with them -- changes nothing measurable under either load: no
interval reaches 1.02, and every one contains 1.00.

**The round trip through ClangIR costs 1.3% to 3.4%**, so the acceptance
fixed in advance, within 2%, is not met on three of the four workloads.
That is a cost of the pipeline before it changes anything, and it means a
transformation applied through it has to be measured against `cir`, not
against `orig`. Measured so, raising 57 globals to a line each -- which
does separate `expanding` from `stats_state` -- changes nothing either.

What these two legs do not show: a gain on a workload neither of them
drives, such as continuous `flush_all` or a busy slab mover; anything about
the arrays and heap objects the analysis does not report; and anything
below about 2%, which ten repetitions on a host with frequency scaling
cannot resolve. Leg 1 shuffled the arm order per repetition; leg 2 rotated
it, so every arm ran in every position but always after the same
predecessor, which leaves any carry-over from one run to the next
unbalanced. Nothing in the data suggests one, and the `hand` result is the
same in both legs.

## What the analysis still cannot say

A `definite` verdict says two accounted-for threads repeatedly touch the
two fields; it does not say they do so *at the same time*. The walk has no
notion of `pthread_join` or of branches that exclude each other, so two
threads started in the two arms of an `if`, or one after the other with a
join between them, are still two threads to it, and main's loops after it
has joined its workers still pair with them. On memcached this does not
arise, since its threads run for the life of the process, but it is a
limit of the verdict, not of one program. So is frequency, which every
pair in the review was decided by, and which no static rule here
estimates.

---

# Retracted, 22 September 2026: the memcached finding does not hold

An adversarial audit of this document overturned the finding it was built
around, `struct stats_state`, on three counts, all checkable from the source:

1. **The struct is 56 bytes.** It fits inside one cache line whole. So
   "every field is contended with every other, density 1.00" is arithmetic
   about a 56-byte object, not a consequence of how the author ordered it.
   Any ordering gives the same graph.
2. **Every write is under one global mutex.** `items.c:404,499` and
   `memcached.c:828,952` all sit between `STATS_LOCK()` and `STATS_UNLOCK()`,
   and `stats_lock` is a single process-wide mutex (`thread.c:80`). The
   accesses are mutually exclusive in time. A core entering the critical
   section already owns the lock's line exclusively; separating the fields
   removes no coherence traffic and adds a second line to the working set of
   every critical section.
   *Corrected 16 October: the lock is not on the data's line, so separation
   can spare a fetch of the data line -- but only when the two fields are
   written by different threads. Here the hot fields are all written by the
   worker pool, so the conclusion stands for a different reason.*
3. **The threads are the same threads.** `conn_new` (`memcached.c:828`) is
   called from `thread.c:624`, inside the worker's own event handler: the
   listener hands the descriptor to a worker, and the worker calls it. It is
   not a separate "connection path" as claimed below.

So the author's comment -- *"Ordered for some cache line locality for
commonly updated counters"* -- is **right**, including for several threads,
and the reading of it below is wrong. Under one global lock, grouping the hot
counters is what you want.

The finding was a true statement about offsets and a false statement about
cost. Everything below the line is kept as written, because a retraction that
deletes what it retracts teaches nothing; read it knowing the conclusion did
not survive.

Numbers in this document were also under correction: a checked-in build
tree made all 34 of LLVM's "sites" artifacts, 41 macro definitions were
counted as sites (which is why two tables here totalled 1,572 and 1,531 and
neither said so), the nesting tracker could not see Allman braces, and the
search for grouping idioms missed the kernel's own `__cacheline_group_begin`.
The recount was done on 16 October; see the section above.

---

# The corpus census: what people pad by hand

The project's next transformation lays out memory by owner: fields written by
different threads are moved apart so their cache line stops travelling between
cores. Before writing the analysis, we counted how often people already do
this by hand, because every hand-written annotation is a place where an expert
has already decided *different threads touch these*, and that decision is the
ground truth the analysis will be measured against.

Nothing here is compiled. The census is a scan of source text, so it covers
codebases the pipeline cannot yet ingest, the Linux kernel included.

## What counts as a site

A site is a **mechanism**: something that tells the compiler to separate the
data. Prose is counted separately and never as a site.

| idiom | example |
|---|---|
| `kernel_aligned` | `atomic_long_t defer_count ____cacheline_aligned_in_smp;` |
| `kernel_group` | `__cacheline_group_begin(rx);` -- one group, counted once |
| `kernel_aligned_to` | `} __aligned(SMP_CACHE_BYTES);` |
| `dpdk_guard` | `RTE_CACHE_GUARD;` |
| `tbb_nfs` | `alignas(max_nfs_size)` |
| `dpdk_aligned` | `struct __rte_cache_aligned lcore_params {` |
| `alignas_interf` | `alignas(folly::hardware_destructive_interference_size)` |
| `alignas_named` | `alignas(CACHE_LINE_SIZE)` |
| `attr_aligned` | `__attribute__((aligned(64)))` on mutable data |
| `alignas_literal` | `alignas(64)` |
| `pad_field` | `char cacheline_pad[64];` |

## Three things that look like sites and are not

Each was found by reading a sample of the hits, not by reasoning in advance,
and each would have corrupted the ground truth before the analysis said a word.

**Macro definitions.** `#define CACHE_LINE_SIZE 64` says the author knows what
a cache line is, not that this struct was split because of one. 2,402 of them,
excluded. *(Recounted 16 October: every preprocessor line is now set aside
before the mechanisms are matched, including the continuation lines of a
multi-line macro, and the 2,760 that mention cache lines are recorded so the
two tools can agree on what was set aside.)*

**Vector-load alignment.** Every one of FFmpeg's `aligned(64)` hits was a
`static const int16_t` table in hand-written MIPS kernels: alignment for SIMD
loads, a different problem with the same spelling. Nobody writes a `const`
table, so it cannot be falsely shared. 50 of them, excluded by requiring the
declaration to be mutable.

**Format padding.** Most `uint8_t pad[7]` in real code describes a hardware
register, a wire protocol or an on-disk record. A sample turned up an AMD GPU
firmware struct, a DPDK command descriptor, a MIPS memory-mapped window and
the XFS log format, none of them about threads. 591 of them, excluded by
requiring the array to be cache-line-sized or cache-line-named, which leaves
67. *(92 once the name test ignores case.)*

## The census

17 codebases, 110,016 source files. *(Recounted 16 October; the September
counts are in the second column.)*

| codebase | files | sites | was | prose |
|---|---:|---:|---:|---:|
| linux | 64,472 | **836** | 773 | 1,916 |
| dpdk | 5,381 | **567** | 573 | 373 |
| folly | 2,344 | 71 | 65 | 132 |
| mysql | 11,387 | 67 | 73 | 113 |
| compiler-rt | 3,472 | 30 | 26 | 22 |
| onetbb | 705 | 25 | 1 | 37 |
| rocksdb | 1,400 | 24 | 11 | 85 |
| parsec | 8,630 | 21 | 6 | 74 |
| abseil | 875 | 6 | 4 | 34 |
| jemalloc | 445 | 6 | 0 | 16 |
| postgres | 2,565 | 5 | 5 | 50 |
| libuv | 369 | 1 | 1 | 2 |
| llvm | 6,940 | 0 | 34 | 66 |
| nginx | 412 | 0 | 0 | 8 |
| leveldb | 133 | 0 | 0 | 0 |
| memcached | 96 | 0 | 0 | 2 |
| sqlite | 390 | 0 | 0 | 3 |
| **total** | **110,016** | **1,659** | 1,572 | **2,933** |

## What it changes

**A count of zero is not a count of no problem.** SQLite and memcached have no
mechanism at all, and those were the two applications the previous plan was
built around. Either they have no false sharing, or they have it and nobody
has looked. The census cannot tell which, and that is the point: this is what
the analysis is for.

**Rich and poor codebases serve opposite purposes**, which the plan did not
distinguish and should:

- A codebase with many annotations has the problem *and has fixed it*. It is a
  good oracle and a poor place to measure, because the headroom is already
  spent. Linux, DPDK, folly and MySQL are oracles.
- A codebase with few annotations has the problem *unfixed*, or not at all. It
  is a poor oracle and the only place a speedup can come from. PARSEC has six
  *(21, recounted, 16 of them in its vendored TBB)* sites in 8,630 files and a literature that keeps finding false sharing in it,
  which is the combination worth measuring.

**memcached did the transformation by hand and left prose instead of a
mechanism.** `memcached.h:431` reads

    * Ordered for some cache line locality for commonly updated counters.

That is field reordering by owner, the transformation this project intends to
automate, performed by a human who then had no way to state it in the
language. It is the clearest single justification in the corpus for doing this
in a compiler.

> **Retracted.** See the top of this file: the accesses are serialised under
> one global lock and the object is 56 bytes, so nothing was being paid.

## Reproducing

```sh
tools/corpus-fetch.sh                    # shallow clones into the corpus dir
tools/corpus-census.py name=path ...     # summary table
tools/corpus-census.py name=path --detail sites.csv    # one row per site
```

`--detail` writes every site with its file, line and idiom, which is the form
the analysis is compared against: precision is how often it agrees with a
site, recall is how much of the set it finds.

---

# Step B: running the analysis over that code

The census says where people have already fought this problem. This step
asks what the analysis says about the same code, which is a different
question and a harder one, because the census reads text and the analysis
has to compile.

## What the analysis is scored on

`tools/wtc-scan.sh` compiles a translation unit to ClangIR under a memory
cap and a timeout, runs `--wtc-false-sharing-diagnose`, and writes one CSV
row per finding with its source location. Both limits are deliberate: on
SQLite's 262,858-line amalgamation the front end reached 22.9 GiB and had
not finished after 12 minutes, where `clang -O2 -c` takes 17 seconds and
251 MB, so a file that runs away is recorded as a refusal rather than
allowed to take the host with it.

## What it found, and what that cost to believe

On memcached the first run reported **527 definite pairs**. Reading them
found three separate ways of being wrong. None showed up on hand-written
tests; each is now a refusal with a test taken from the case that exposed
it.

| what was wrong | the case that showed it | now |
|---|---|---|
| **cold pairs** | `settings.lru_maintainer_thread = false` on the shutdown path: one store, one invalidation, no cost | a verdict needs repeated access on *both* sides |
| **placeholder threads** | scanning one file of a many-file program, every externally visible function nobody calls there becomes its own thread; pairing two of them was most of the 527 | a thread must be one the module accounts for |
| **the start-up phase** | memcached's getopt loop writing `settings.maxbytes` paired against every worker that later reads the struct: 176 findings, one phase | accesses before any spawn cannot contend |

527 → 176 → **20**, with 422 more pairs set aside as cold and 6,956 field
accesses on objects the analysis could not identify. *(Every count in this
section was taken from a walk that had stopped at 10,000 function
instances without saying so, and the 6,956 counted one access once per
call string. See the top of the file for the recount.)* That last number is the
honest measure of how far it reaches on real code, and it belongs beside the
findings rather than in a footnote.

A fourth refusal came later, from a report that names both accesses instead
of one: the start-up phase was ending at main's first function pointer,
because any indirect call was assumed able to spawn. It can only spawn if
some address-taken function in the module can, so that is now asked. The
count moved between these changes and pthread_create being recognised, and
the two were not measured apart -- a sequence of tuning steps without an
isolating measurement between them, which is worth saying rather than
presenting the last number as if it followed from the last change.

## The finding worth the exercise  *(retracted -- see the top of this file)*

`memcached.h:430`, above `struct stats_state`:

> Ordered for some cache line locality for commonly updated counters.

The author thought about cache lines here and grouped the hot counters
together. That is right for one thread and backwards for several: it puts
them in the *same* line. `curr_items` is written by workers (`items.c:404`,
`items.c:499`), `curr_conns` by the connection path (`memcached.c:828`,
`memcached.c:952`), and the analysis reports the pair.

Grouping for locality and grouping by owner are opposite instructions, and
nothing in the language lets you say which you meant.

## The limit this step measured

Findings appear only where `main` and the contended code sit in one
translation unit. That is not a defect of the implementation, it is the
refusal working: in a file that contains neither, every function is a
placeholder and no pair can be claimed.

| codebase | scanned | refused | findings |
|---|---:|---:|---:|
| memcached, per file | 31 | 20 | 0 definite, 460 possible *(was 20 definite, 422 cold)* |
| memcached, amalgamated | 1 | 0 | 155 definite, 503 possible, 349 cold *(was 397 definite, 546 possible)* |
| libuv | 44 | 18 | 11 possible *(was 32 scanned, 1 possible; the recount scans `src/` and `src/unix/` whole)* |
| leveldb | 28 | 13 | none *(was 11 scanned, 2 possible; the recount scans `db/`, `util/` and `table/` without tests)* |
| redis | 0 | 3 | needs its vendored dependencies built first |

libuv's refusals are its AIX, BSD and Darwin files, which is the scanner
being right. leveldb's two were `Slice`'s return slot, an escaping stack
object; since 16 October a stack slot in a function that runs more than once
gives no verdict at all, so they are gone.

So the next step is not more files. It is whole-program input, and that was
built: 27 of memcached's translation units in one file, after giving its
headers the include guards they lack (memcached.h has none, which is
harmless when each .c file stands alone and fatal when they are one) and
setting aside the platform-only files, a second implementation of the
slab-automove interface that declares the same type names differently, and
one static that collides with an external of the same name.

It does turn possibles into answers -- and it also shows the limit that no
static frequency estimate crosses. The analysis can tell an access that
repeats from one that happens once. It cannot tell a frequent one from a
rare one: a command handler reachable from many threads counts as repeated
whether its command arrives a million times a second or never. That is the
same wall the neighbouring project measured from the other side, where the
sites their analyses could remove turned out to be dynamically cold, and it
is why the next thing worth building is a way to ask the program how often,
not another threshold.

---

# What the annotations separate, and which transformation reaches them

Counting the sites says how much of this people do. It does not say what to
build. That turns on one question: when someone fights false sharing by
hand, are they pushing two *fields* of one object apart, or two *objects*
apart? The two need different transformations, and only one of them is what
`alignas` can express.

The rule is one line: **the annotation separates fields exactly when the
thing it annotates lives inside a record.** `tools/corpus-classify.py` reads
that from the file's nesting rather than from the line, because the same
spelling means both things.

| codebase | field | object | was |
|---|---:|---:|---|
| linux | 455 | 381 | 402 / 348 |
| dpdk | 167 | 400 | 159 / 405 |
| mysql | 34 | 33 | 58 / 7 |
| folly | 30 | 41 | 28 / 37 |
| parsec | 21 | 0 | 5 / 1 |
| rocksdb | 18 | 6 | 11 / 0 |
| compiler-rt | 14 | 16 | 10 / 16 |
| onetbb | 13 | 12 | in "others" |
| llvm | 0 | 0 | 0 / 34 |
| others | 9 | 9 | 2 / 8 |
| **total** | **761 (46%)** | **898 (54%)** | 675 / 856 (44% / 56%) |

*(Recounted 16 October. The nesting tracker now sees a brace on its own
line and skips comments, string literals and preprocessor lines. The
largest shift, MySQL's, from fields towards objects, has not been checked
site by site.)*

Reading the line instead of the nesting gets this wrong, and the first
version of the script did. The kernel's idiom for separating *groups* of
fields is a nested anonymous record, so the annotation lands on a closing
brace:

```c
struct kioctx {
    ...
    struct { atomic_t reqs_available; } ____cacheline_aligned_in_smp;
    struct { spinlock_t ctx_lock; struct list_head active_reqs; }
        ____cacheline_aligned_in_smp;
};
```

That is `fs/aio.c:136-150`, and it is field separation performed by hand,
with the comment above it explaining which counter is which. Counting those
closing braces as whole objects put Linux at 267 fields against 483 objects;
reading the nesting puts it at 402 against 348 *(455 against 381,
recounted)*.

## What it changes

**Neither case dominates, and the split is a property of house style.** The
kernel and MySQL separate fields; DPDK, which puts `__rte_cache_aligned` on
whole structs, separates objects; LLVM separates only objects. So the two
transformations are of comparable reach, not a flagship and a follow-up:

- **reordering fields by owner** reaches 44% *(46%)*, and costs nothing when it
  works, where the hand-written version always costs a line of padding;
- **placing allocations by owner** reaches 56% *(54%)*, and is the half the language
  cannot express at all: `alignas` lives inside a type, so two objects from
  two separate allocations have no syntax to keep them apart.

The second is also the half with no prior art to compare against, static or
dynamic, because a profiler reports an address and an address does not say
which allocation site produced it.

---

# Which objects reordering can actually help

A finding says two fields share a line and two threads touch them. It does
not say the layout can be fixed. Read one object's findings as a graph --
a field is a vertex, a reported pair an edge -- and the question becomes
whether the fields can be split across lines so that no edge stays inside
one. `tools/corpus-contention.py` measures the density.

*(The table below is from the truncated walk that never saw the workers.
Recounted on 16 October from the definite pairs: `@stats` 23 fields, 78
edges, 0.31; `@slab_rebal` 14, 25, 0.27; `@stats_state` 11, 49, 0.89;
`@active_crawler_mod` 4, 3, 0.50. `@settings` has no definite pair.)*

On amalgamated memcached, all 943 findings sit on 14 objects, and they
divide exactly along the rule the pass was built with: every global is
`definite`, every stack slot is `possible`, because a slot is a different
object in every invocation. The four globals:

| object | fields | edges | density | max degree |
|---|---:|---:|---:|---:|
| `@settings` | 68 | 214 | 0.09 | 17 |
| `@stats` | 28 | 89 | 0.24 | 7 |
| `@slab_rebal` | 13 | 23 | 0.29 | 12 |
| `@stats_state` | 12 | 66 | **1.00** | 11 |

`@stats_state` has 66 edges on 12 fields, and C(12,2) is 66: **every field
is contended with every other**. No ordering separates a complete graph.
Whatever arrangement is chosen, the fields stay adjacent.

That is the object whose header says *"Ordered for some cache line locality
for commonly updated counters"* -- so the best finding in the corpus is one
the flagship transformation cannot fix.

> **Retracted.** The density is 1.00 because the struct is 56 bytes, not
> because of the author's ordering, and the pairs cost nothing because every
> access is under one global lock. What remains for it is padding,
which costs a line per field, or a copy per thread summed on read, which
changes the program rather than its layout.

The other three are sparse enough that a partition by owner plausibly
exists, `@settings` most of all at 0.09.

**This is the reason to measure before building.** The plan had reordering
as the flagship and padding as its fallback. Density says they are answers
to different objects, not better and worse answers to the same one, and
that an implementation which only reorders would leave the clearest case in
memcached untouched. It also gives the transformation a cheap precondition
to check: compute the density, reorder below some threshold, pad above it,
and refuse when neither pays.

---

# What shape the transformation should take

Three measurements, taken before writing it, say what it has to be.

> **Withdrawn 16 October.** Recounted, 90% sit on one member and 10% mark a
> group, three ways: `__cacheline_group_begin_aligned` (25), an annotated
> closing brace (27) and an annotated opening brace (22). 153 files carry
> several, at most 17. People mostly mark boundaries, but grouping is not
> rare, and a transformation that can only emit single-member alignment
> would not reproduce one annotation in ten.

**People mark boundaries, they do not group.** Of the 675 field-level
annotations, 648 (96%) sit on a single member -- `spinlock_t lock
____cacheline_aligned_in_smp;` -- and 27 wrap a group of members in a
nested anonymous record. And 300 files carry several such annotations, one
of them 24, so a partition is usually expressed as a series of boundaries
rather than as groups.

That matters because of what the annotation does. `____cacheline_aligned`
on a member aligns its *start*: padding goes in before it, and the field
order is untouched. So the form people actually write is one a compiler can
emit without reordering anything.

**Reordering is the cheaper answer, not the general one.** It costs no
memory where it works, and where the contention graph is dense it does not
work at all: `@stats_state` is complete, and no arrangement of a complete
graph separates it.

**So the transformation is three answers, chosen by a precondition it can
check:**

| the object looks like | do | costs |
|---|---|---|
| sparse contention, fields partition by owner | reorder | nothing |
| dense contention, or the type's order is fixed by ABI | align selected members | a line per boundary |
| complete contention | refuse, and say so | nothing |

Aligning members is also the safer thing to build first, and not only
because it is what people write. It preserves field order, so every offset
only grows; a `static_assert` on `sizeof` and `offsetof` per touched type
checks it completely; and it does not need the record type rebuilt across
the module, which reordering does and which is where the real work is.

Reordering is then an optimisation on top, applied where density says it
pays, rather than the thing the whole transformation rests on -- which is
how the plan had it, before any of this was measured.

---

# The inter-object half, built and measured: it justifies a flag, not an analysis

A global's alignment is an attribute of the operation, not part of its type:

```
cir.global external @s = #cir.zero : !rec_Stats {alignment = 8 : i64}
```

so raising it is one attribute, with no type rebuilt and no offset moved,
and raising an alignment is safe by construction -- the linker takes the
maximum across translation units and nothing inside the object shifts. That
made the inter-object half look like the cheap one to build, and the census
made it the larger one at 56%. So it was built: `--wtc-align-by-owner`
raises the alignment of every global that one thread repeatedly writes while
a different thread repeatedly touches another, and counts the bytes.

On amalgamated memcached:

| | globals | padding |
|---|---:|---:|
| all mutable globals (1,303 of the 1,444 are constants) | 141 | 7,890 B |
| what the analysis picks | **73** | **4,254 B** |

*(Recounted 16 October, on the whole walk with the workers visible: **60**
globals, **3,489 B**. The argument below holds with the new numbers -- the
analysis picks 43% of the mutable globals and saves 4.4 KB against a flag --
and what the pass is worth when run is in the top section.)*

**The analysis selects half the mutable globals and saves 3.6 KB** *(43% and 4.4 KB, recounted)*. The
guarantee is identical either way: after the pass no two contended globals
share a line. A build flag that aligned all 141 would give the same
guarantee for 7.9 KB instead of 4.3 KB, in a program that uses gigabytes.

That is a negative result about the design, and it was only visible by
running it. *(The next claim is withdrawn: the pass remembered only the first
partner it found for each global, so "pairs with" was an artifact of the
order of the walk.)* Seventy of the seventy-three pair with the same global,
`@current_time` -- memcached's clock, written once a second by the timer
thread and read by everything -- so the selection is really *which globals
are hot*, not *which two would collide*. Which two collide is the linker's
decision, taken after every translation unit has spoken, and no
single-translation-unit analysis can answer it.

## Where the argument survives

Re-reading `vision.md:129-132`, it does not say globals:

> two objects owned by different threads land on different cache lines even
> when they are separate **heap allocations**, which `alignas` cannot
> express

Heap placement is decided per call, by the allocator, at run time. No flag
reaches it and no annotation can name it, so the compiler rewriting the
allocation site is the only thing that can -- and that is a claim a flag
cannot match, unlike the one for globals. It is also unbuilt and unmeasured
here, so it is where the argument survives rather than where it is won.

## What this settles about the two halves

| half | selective? | why |
|---|---|---|
| fields of one record (44%; 46% recounted) | **yes** | offsets are known at compile time, so the analysis says exactly which pairs share a line |
| separate globals (56%; 54% recounted) | no | placement is the linker's; a flag gives the same guarantee for twice the memory |
| separate heap objects | unmeasured | placement is the allocator's; nothing but the compiler reaches it |

So the field case is the one where an analysis beats a flag, even though it
is the smaller share of practice and needs the record type rebuilt across
the module, which is the expensive machinery. Two messages ago that
conclusion was the other way round, on the strength of the alignment
attribute being cheap to write. Cheap to perform and worth performing are
different questions.
