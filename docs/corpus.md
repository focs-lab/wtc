# Status, 17 October 2026: recounted twice, and no reported pair survives review

This file was written in September, audited, and found wrong in places; the
fixes were audited again on 16 October and found wrong in more; and the
second round of fixes was audited once more the same day, which found
fifteen defects in the analysis, fourteen errors in the numbers and sixteen
claims the sources do not support. This section says where it stands after
all three. The sections below keep what they said, with corrections marked
where they were made, because a retraction that deletes what it retracts
teaches nothing.

**The result of this phase is negative, and narrower than it first looked.**
On amalgamated memcached the analysis reports 246 definite and 2,610
possible pairs, 1,131 of the possible on stack slots of command handlers
whose address goes to a helper, where two invocations are two objects. 156 of the definite pairs and 852 of the possible sit on five
globals, every one of which was read against the source, and no pair there
describes traffic that separating fields would remove: each is decided by
how often one side runs, which the analysis cannot tell; the 72 definite
pairs on extstore's engine object and the 21 on the crawler's module,
which the one-of round and the member-address round added, read the same
way (below). Two measurements
under load do not overturn it, within what ten runs resolve: applying the
verdict by hand gives throughput within 0.7% of the original on every
workload, with 95% intervals from -3.2% to +2.0%. Of the other possible
pairs, 490 sit at elements of arrays and heap objects -- the per-thread
`threads[]` and `itemstats[]` among them -- which the analysis places since
the October round on heap objects and elements (below) but cannot tell
apart, 5 come through pointers that may name one of several objects, and
16 are on the extstore engine; none of these was read. What it still
cannot place, 4,103 accesses through pointers it cannot follow, the slab
allocator's items and the per-connection state among them, was not
examined either, so this is not a claim that memcached has nothing to fix.
Counted at the source the walk stopped at, most of those are the results
of calls that are not allocations and the `void *` parameters of callbacks
nobody in the module calls, not the slots and fields they sat in.

## What was wrong, and what it is now

Every number moved twice: once on 16 October when the walk was found to
have stopped silently, and again the same evening when the fixes were
reviewed. Both columns are kept.

| what | September | first recount | **now** | why it moved |
|---|---:|---:|---:|---|
| source files scanned | 112,831 | 110,016 | **98,344** (+11,672 test files set aside) | a checked-in build tree was scanned; then test directories and `*_test` files were, and a fixture aligned to 64 is not a decision about two threads |
| sites | 1,572 | 1,659 | **1,623** (1,546 in the v5 recount of 17 October on a refetched corpus at later commits, which mixes tool fixes with source drift; see the census section) | directives, comments and build trees were counted; seven idioms were missed; then tests were set aside (-36 net of the new idioms) |
| precision of a site | -- | "near 75%", from 40 read | **74% [65%, 82%]**, 100 read and labelled in `corpus-precision-sample.csv`, six labels changed by an independent second reader | the misses are DMA and firmware layouts, SIMD tables, read-only lookup tables, hot/cold locality splits, habitual alignment of read-mostly structures and a SPARC trick; `kernel_aligned` 26 of 33, `alignas_named` 13 of 22, `dpdk_aligned` 13 of 18 |
| what the census misses | -- | -- | **from about a tenth to about half of what it finds**, per codebase (DPDK 0.12, folly 0.19, compiler-rt 0.31, postgres 0.40, MySQL 0.53): folly ~13 against 71 found, MySQL ~39 against 74, compiler-rt ~6 against 18, DPDK ~62 [11, 305] against 539, postgres 2 against 8; from 120 candidates read against the previous census, ten of which the current one finds; `corpus-recall-sample.csv` | spellings with no pattern: a padded type (`PFS_cacheline_atomic_*`), a size macro (`LWLOCK_PADDED_SIZE`, `BUFFERDESC_PAD_TO_SIZE`), a group marker (memif), `ALIGNED(` split across lines, and a union of an atomic with a line-sized buffer |
| fields / objects | 675 / 856 (44% / 56%) | 761 / 898 (46% / 54%) | **828 / 795 (51% / 49%)** (v5: 785 / 761, 51% / 49%) | the nesting tracker could not see Allman braces; then it ended a record at the `)` of `struct alignas(X) Name {`, which made every member of such a record an object: 23 sites in MySQL's `log0sys.h` and 8 elsewhere move to fields, and MySQL's 34 / 33 is 66 / 8 with the nine sites the second census found |
| field annotations on one member | 648 of 675 (96%) | 687 of 761 (90%) | **747 of 828 (90%)** (v5: 703 of 785, 90%) | two of the three ways the kernel writes a group were missed; `__cacheline_group_begin(x) ____cacheline_aligned` is a group too |
| files with several field annotations | 300, at most 24 | 153, at most 17 | **165, at most 23** (`log0sys.h`) | recounted from each classification |
| memcached pairs, amalgamated | 397 definite, 546 possible | 155 definite, 503 possible, 349 cold | **159 definite, 1,363 possible, 0 cold after the one-of round (156 / 1,335 / 0 before it); 365 of the possible would be cold but for accesses the walk could not place or placed only as one of several objects** (155 / 852 / 0 after the fourth and fifth rounds of fixes, 136,673 instances; the heap-and-element round adds one definite pair on `slabclass[0]` and 483 possible pairs at elements the analysis cannot tell apart, 137,526 instances) | the walk had stopped at 10,000 instances and never saw the workers; then "cold" stopped being claimed for a thread with unplaced accesses, which every memcached thread has; then heap objects and array elements were placed at all |
| field accesses the analysis cannot place | 6,956 | 4,493 | **4,103** after the DPDK round (4,265 after the one-of round, 4,377 before it), with 718 more at elements of arrays it places but cannot tell apart, 71 on stack slots that never escape, and 51 placed as one of several objects | counted once per operation, not per call string; then atomic and bit-field accesses were counted at all (4,496); then accesses to array elements, to the whole of an object through a cast, and by memset and memcpy, none of which had been counted as accesses before (5,106); and a slot whose address never escapes is now told apart, which no slot ever was, since the escape check answered yes on every exit; then heap objects, elements at constant and per-thread indices, and the whole of an object through a pointer the walk resolves were placed |
| memcached pairs, per file | 20 definite, 422 cold | 0 definite, 460 possible | **0 definite, 490 possible** | a placeholder thread gives no definite or cold verdict; then atomics and bit-field stores became accesses |
| leveldb, per file | "none" | 0 | **0 pairs in the 11 files that compile** (2 refused: the Windows environment and a test helper) | the earlier 0 was a build artefact: all 13 files were refused, ten of them on a missing `LEVELDB_PLATFORM_POSIX`, the threaded ones among them |
| globals `--wtc-align-by-owner` picks | 73, 4,254 B | 60, 3,489 B | **60, 3,489 B**, a different 60 | `stderr`, a libc declaration the pass must not touch, is out; `logger_gid`, written atomically, is in. The comparison point counted two declarations among its 141: 139 defined mutable globals, 7,778 B |

The idioms added in the first recount were the four kernel ones, DPDK's
`RTE_CACHE_GUARD` (8 once tests are set aside), oneTBB's
`alignas(max_nfs_size)` (18), and project macros that align to a line (39
now, with compiler-rt's `ALIGNED(SANITIZER_CACHE_LINE_SIZE)` and MySQL's
`Cacheline_padded<T>` added in the second). The plain
`__cacheline_group_begin` is *not* counted: it is a zero-length marker that
aligns nothing. Allocation-time alignment -- `rte_zmalloc(..., RTE_CACHE_
LINE_SIZE)`, `palloc_aligned`, `ShmemAlloc` -- is a separation mechanism
of a different kind, decided per call rather than per type, and the census
does not count it; ten of the twenty postgres candidates were of that kind.

What the corrections do to the conclusions drawn below: people group
fields as well as mark single boundaries (10%, not 4%); fields and objects
are separated about equally often, not 44 to 56; the memcached counts the
density and alignment sections stand on came from a walk that had stopped
partway; and "1,623 sites" is a count of spellings whose precision is 74%
and whose misses are of the same order as its errors, so no claim here
rests on its third digit. Test and benchmark code is set aside, though
about two thirds of the 110 sites set aside are themselves thread
decisions inside stress tests and benchmarks; and 66 sites in test
applications the rule does not match (DPDK's `app/test-*`, rocksdb's
`*_bench.cc`, the kernel's `lib/test_*.c`) are still counted, as are six
lines of a printf format in `raid6/mktables.c`.

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
- **The third review, the same evening**, ran the passes over small
  programs with known answers and found the verdicts wrong both ways.
  Over-claims: a thread-local global was one object for every thread
  (`thread-local-is-private.mlir`); two spawns in the two arms of an `if`
  were two certain threads (`spawn-in-either-branch.mlir`); a stack slot of
  a function called twice was one object (`stack-slot-per-call.mlir`); a
  `goto` made every access in its function "repeated"
  (`goto-is-unknown.mlir`); a store of a whole record and a store of one of
  its members were two fields of one line (`overlap-is-true-sharing.mlir`);
  main's start-up loop paired against a worker that itself spawns, because
  the indirect-call check counted a spawn entry as an address taken
  (`worker-that-spawns.mlir`). Under-claims: a thread spawned from a
  helper's local vanished (`entry-through-helper-slot.mlir`); atomic
  read-modify-writes, bit-field stores and `cir.copy` were not memory
  accesses at all, so the textbook pair of atomic counters produced no
  verdict (`atomic-counters.mlir`, `atomic-globals.mlir`); a recursive
  function ran "once" and its pair was cold (`recursion-is-repetition.mlir`);
  a thread with an access the walk could not place still made a pair cold
  (`cold-needs-resolved-thread.mlir`). And `--wtc-align-by-owner` raised the
  alignment of `extern` declarations, globals in named sections,
  thread-local and weak globals (`cannot-move.mlir`), and both passes
  asserted on a global of incomplete record type (`incomplete-root.mlir`);
  the instance cap did not count roots (`cap-counts-roots.mlir`). Three
  tests that named a rule never reached it, and five checked a verdict but
  not the summary; they do now.
- A record-padding pass, written after the September measurements and
  never committed, is not kept. Layout cannot be changed after the front
  end: `sizeof` arrives already folded into a constant, so
  `malloc(sizeof(struct S))` keeps the old size while the struct grows,
  which is a heap overflow with no remaining evidence to refuse on. Layout
  changes belong in the source, before the front end runs.

Several tests that passed whatever the pass did now check a summary line
the pass always prints, and counts are anchored so that `0 definite` cannot
match `10 definite` -- the tail counts of the line by the separator before
them, since the October audit found `{{.*}}1 field accesses` matching `11`. One test compiles a program, transforms it, links,
runs it, and asks the running program where its two counters landed
(`align_by_owner_runs.c`); before it, no test executed anything this
project had changed.

## Heap objects and array elements

Until October the analysis placed a member access only when the base of
its `cir.get_member` chain reached a global or a stack slot. Everything
else -- an object `malloc` returned, an element `arr[i]`, the struct a
thread was handed at `pthread_create` -- was counted as an access it could
not identify, 5,106 of them on memcached against 1,007 pairs. The round of
work recorded here places them, and says what is still not placed.

**What is placed now.** A heap object is named by the allocator call that
made it (`calloc() in @memcached_thread_init`), reached through a stack
slot written once, a parameter resolved through the call chain, a pointer
global the module stores exactly once and never names elsewhere, or a
wrapper whose every return hands back one allocation (Phoenix's `CALLOC`,
any checked `malloc`, with or without a `NULL` on its failure path). A
`realloc`, a second store to the global, a `global_view` or an alias of it,
and a pointer kept in a field of another object all refuse. Whether the
allocation runs once is read from the call graph: once in `main`, or in a
function called from one site that itself runs once. An element step with
a constant index is so many bytes in. The element a spawn loop hands each
thread, `pthread_create(..., &args[i])` with `i` the loop's own variable,
is *one element per thread*: the thread's offsets are relative to its
element, and when the loop steps by one, spawns on every iteration, is the
only repetition around the spawn and sits in a function that runs once,
neighbouring elements have neighbouring threads. Any other index is an
element the analysis cannot read: the access is placed on its array,
labelled *element not known*, and no pair there is more than possible.
The alignment of a heap object is what the allocator promises -- 16 bytes
for `malloc` and `calloc` on x86-64 glibc, settable with `--heap-align`,
the operand of `aligned_alloc` and `memalign` when it is a constant -- and
an element starts where the array's alignment and the element size let it.

**Verdicts across elements.** With one element per thread, the tail of
one thread's element and the head of the next thread's share a line unless
the element is a multiple of a line and the array is line-aligned. A field
hot-written by the owner of one element against a field the owner of the
next hot-touches is definite; against a once-written field it is cold;
padding each element to a multiple of a line *and* aligning the array to a
line removes it, and the remark says so. Two fields of one element touched
by its one owner are no pair. Where an element is not known, the same
field pairs with itself -- on two neighbouring elements it sits on one
line -- and two fields may be on one element or on two; both possible.

**Phoenix.** The dynamic detectors' canonical example is Phoenix's
`linear_regression`: one 64-byte results struct per thread, in a `calloc`'d
array, hot-written by its thread. Built with `-DNDEBUG` (its `CHECK_ERROR`
macro otherwise evaluates `pthread_create` twice, the second time on the
failure path, and the analysis, which reads no conditions, then sees two
threads on each element), the analysis reports 17 definite pairs across
elements -- every pair of {`SX`, `SY`, `SXX`, `SYY`, `SXY`} hot-written by
one thread against {`points`, `num_elems`, `SX`, ...} hot-touched by the
next that can share a line given a 64-byte element aligned to 16, with the
position counts the layout gives by hand, nothing more and nothing less --
and places every access the workers make to that array; the 11 accesses it
leaves unplaced are the workers' reads of `points[i]` through a pointer
field and `main`'s `argv[]`. The other Phoenix programs that compile
through ClangIR, with the same build:

| program | definite | possible | cannot place | at elements not told apart |
|---|---:|---:|---:|---:|
| `linear_regression` | 17 | 0 | 11 | 9 |
| `histogram` | 0 | 0 | 38 | 9 |
| `kmeans` | 0 | 1 | 30 | 14 |
| `matrix_multiply` | 0 | 1 | 18 | 1 |
| `pca` | 0 | 1 | 8 | 21 |
| `string_match` | 0 | 0 | 14 | 13 |
| `word_count` | 0 | 21 | 31 | 51 |
| PARSEC `streamcluster` | 0 | 10 | 215 | 28 |

`histogram`'s known false sharing is between the three per-thread
histograms each worker allocates for itself, neighbours by the allocator's
choice, which the analysis does not model. `word_count`'s is on `use_len[]`
and `length[]`, indexed by a thread id the spawn loop stores into each
worker's argument struct, which the walk does not yet read through a field;
its 21 possible pairs are on the argument structs, allocated one per
iteration of the spawn loop, so "many objects". Neither is a finding yet.

**memcached, recounted.** 156 definite and 1,335 possible pairs. The one
new definite pair is `slabclass[0].slabs` against `slab_list`, written in
`slabs_prefill_global` and read by the global page pool, now that
`p = &slabclass[0]` resolves. The 483 new possible pairs are all at
elements the analysis cannot tell apart: 295 on the heap `threads[]`
(`calloc() in @memcached_thread_init`, the per-thread `stats` fields
written by each worker at an index the walk does not read), 87 on
`itemstats[]`, 55 on `crawlers[]`, 28 on `slabclass[]`, and 18 on eleven
other arrays, one of them an allocation that may run more than once. None
has been read against the source. 4,377 accesses remain unplaced, 678 are
at elements not told apart, and 71 are on stack slots that never escape.
The alignment pass is unchanged: it raises globals only.

**The audit of this round.** Three independent readings of the five
commits -- the walk, the diagnosis pass, and the claims in commit messages
and tests -- found 33 defects, 11 each. The ones that could make a verdict
false, each closed with a test that fails on the code before the fix: a
thread spawned in a loop stands for many threads, and every access of it
was "repeated", so a worker that wrote its element once was a sustained
pair with its neighbour (what repeats on an owned element is what repeats
inside the thread); "the two threads that own them" was said of a loop
stepping by two, spawning under an `if`, nested in another loop, in a
function that runs twice, or indexing with a value computed before it; a
wrapper that returned its allocation and also stored it in a global gave
the object two roots, and a hot write under one name let the pair under
the other be cold; a `global_view` or an alias of a pointer global was a
second way to write it that the single-store rule did not see; an entry
handed to the spawn through an integer round trip was neither resolved
nor a root, and its thread was lost; a recurrence the walk visits and does
not expand reaches objects the expanded one did not, so its thread blocks
cold. Missed findings closed in the same round: two spawn loops over one
array never met, nor did `main`'s constant-index reads meet the per-thread
accesses; a whole-object access through a pointer, `memset` of a heap
object or `*me` of a per-thread counter, was dropped before placement;
nested unknown steps judged neighbours by the outer element only; a pair
on an allocation that may run more than once was dropped without trace.
The claims audit also found the remark "padding each element to a line
removes it" false on the very Phoenix case, since `calloc` aligns to 16,
and two commit messages overstating what was placed; both are reworded.

### One of several objects

A pointer loaded from a stack slot, a pointer global or a field written
in several places was, until this round, an access the analysis could
not identify: on memcached 1,521 accesses through such slots, 984 through
fields and 26 through globals, more than half of what was unplaced.
Every store into such a place names a value, and when each of those
values resolves, the loaded pointer is *one of* the places they name. The
walk now lists the stores into a slot (under its own name or a bitcast
of it, in the same instance), into a pointer global nothing else names,
and into a (record, field) pair anywhere in the module, resolves each
stored value where its store runs -- in every instance of the storing
function, or without a call string when it has none -- and hands back
the set. An access through a set is recorded on every object in it, apart
from the accesses that name the object: it makes a pair possible, "through
a pointer that may name one of several objects, this among them", never
definite, and a repeated one keeps a pair on that object from being cold,
as a repeated unplaced access does. A set of one is that place, not a
set. A list says nothing -- and the load stays unplaced, counted under
the source the walk stopped at -- when one source cannot be placed, when
the record is written whole (assigned, copied, initialised, seen through
a view of another type, handed by pointer to a function without a body
or as `void *` to any function, or a union), when a field is set from
itself, or when there are more than 8 sources or stores in more than 64
instances.

**memcached.** 159 definite and 1,363 possible pairs, 0 cold; 4,265 accesses unplaced, 703 at elements not told apart, 71 on private slots, 51 placed as one of several objects; 365 of the possible would be cold but for unplaced or set-placed accesses. The possible pairs: 852 on the five globals as before, 490 at elements not told apart, 5 through sets on `slabclass[]`, and 16 on extstore's engine whose once-writer has unplaced accesses. The 110 accesses placed by sets reach
extstore's engine object, `calloc()` in `extstore_init`, with three
definite pairs: `pages`, a pointer set once at start-up and read by every
IO thread per request, beside `wbuf_stack` and `io_stack`, pushed and
popped under `e->mutex` once per write buffer by the storage writer; and
`stats.objects_read` beside `stats.bytes_read`, both written under
`stats_mutex` by the IO threads. The first is a layout pair with one
invalidation per write buffer of megabytes, the second is lock-serialised:
the same class as the `@stats` pairs, nothing a workload would feel. The
five possible pairs through sets are on `slabclass[]`, under
`slabs_lock`. Counted at their sources, what remains unplaced is no longer
mostly slots: 1,183 through a field whose list says nothing, 14 through a global, 324 through a slot whose address goes elsewhere, 1,223 through a parameter of a root nobody is seen to call, 932 through the result of a call that is not an allocation, 62 past the caps, 527 otherwise. The frontier is the result of a call that is
not an allocation and the parameter of a callback nobody in the module
calls -- libevent's `void *arg` -- not the slot or the field it sat in.

**Phoenix and PARSEC, against the published object lists.** Of the five
objects the dynamic-detector papers name that a layout analysis can
reach, the analysis names four: `linear_regression`'s per-thread array
(17 definite pairs across elements), `string_match`'s per-thread argument
array (possible, element not known), and `streamcluster`'s `work_mem`
and `switch_membership` (possible on `malloc()` in `pgain` and `calloc()`
in `pkmedian`, element not known, allocation run more than once);
`word_count`'s `use_len[]`, indexed by a field the spawn loop sets, waits
on reading that index. `histogram`'s known pair lies between two
allocations, which is the allocator's placement and out of reach. Nothing
the papers call insignificant is definite here; `kmeans`'s `sum` objects,
true sharing by the papers' reading, are possible on an allocation that
runs more than once.

**The audit of this round.** Two independent readings of the commit, the
walk and the diagnosis with its claims, found fifteen defects, each
closed with a test that fails on the code before the fix. The
verdict-changing ones: a store through a bitcast of the slot was admitted
by the escape check and missing from the list (a definite pair on the one
object listed); a record global's initialiser, a store through a
`char *` view of the record, a union member written beside the one read,
and the record's address leaving as `void *` through a local or a
forwarding function were writes no listing saw; a candidate resolved in
the storing thread's instance kept that thread's own element and the
collapsed set borrowed owner-ness from the loading thread (four definite
pairs across elements on a handle every worker had published); and the
widest access per thread was recorded before the set branch, so a 64-byte
copy through a set widened an 8-byte direct write to a definite pair.
Also found: a chain of locals assigned on two arms resolved in 2^n time;
the reason and location printed for a pair judged by any direct access
rather than by role; "accesses it could not place" said of a placed one;
a test that could not tell a dropped offset from a kept one.

### The DPDK round: a member's address, constants, allocation sets

Running the analysis on a DPDK test, to score it against DPDK's own
cache-line annotations (the oracle below), needed four things, each with
a test that fails on the code before it. DPDK's EAL has no pthread_create
and no malloc: `rte_eal_remote_launch(f, arg, lcore)` is read as a spawn,
`rte_malloc` and its forms as allocations with their alignment operand,
and the ring and stack constructors as allocations -- a reading of the
API, stated as such. A record written from a constant that names no
object, `= {0}` or `= { .n = 8 }`, which the front end emits as a copy
out of a private constant, no longer marks the record written whole; on
the ring test one such zeroing cost 91 unplaced accesses. A wrapper that
hands back one of several allocations, `test_ring_create` choosing
`rte_ring_create` or `rte_ring_create_elem`, places its result as one of
its sites, through the `__retval` slot every return stores to. And the
walk steps through a member's address handed on as an argument,
`__rte_ring_headtail_move_head(&r->prod, &r->cons, ...)`: before, every
access through such a parameter was "otherwise". A `list-accesses`
option prints a remark per field access, where it went or why not, and
the notes on a possible pair carry both offsets; that listing is what
found the three gaps.

Three more rules, from the stack test (each with a test that fails on
the code before it): a store in a nest of `cir.scope` and
`cir.cleanup.scope` under the slot's block is the slot's one store, since
such a scope runs exactly once when reached and the front end at -O2
puts every initialiser in one, and a lifetime marker is no use that ends
a listing; an untyped view of a record is followed into the function it
reaches -- a spill to a slot, a cast back to the record's type, a call to
a function with a body whose parameter is then followed the same way, to
a depth of four, and a call by function pointer when every caller hands
in a function's address -- instead of marking the record written whole;
and a possible pair on a heap object or a slot looks for two threads as
its witness before it accepts one thread standing for many, where the
first witness found decided and a thread writing both fields came first.

**memcached after.** 246 definite, 2,610 possible (1,457 before the
witness rule; 1,131 of the new ones on escaping stack slots, each with
the caveat that two invocations are two objects, and 252 pairs that
were "by one thread that stands for many" now have two threads), 0
cold; 4,103 unplaced (1,215 field, 14 global, 324 slot, 1,423 root parameter, 872
call result, 62 past the caps, 193 otherwise), 718 at elements not told
apart, 51 through sets; 301 of the possible would be cold but for
unplaced accesses. The 87 new definite pairs all come through the
member's address: extstore spawns its IO threads with `&e->io_threads[i]`
and each reads the engine from `me->e`, the one store into that field;
the crawler hands `&active_crawler_mod.c` to the routines that write and
release its client. Read against the source: 69 on the engine --
counters under `stats_mutex` beside each other (lock-serialised, the
`@stats` class); `page_free` and `version`, set under `e->mutex` once per
page of tens of megabytes, beside `io_depth`, `page_size` and the other
fields read per request (a layout pair with one invalidation per page);
and `wbuf_stack`/`io_stack`, pushed in a loop of `extstore_init` that
runs in main after the worker threads exist, against the IO threads
`extstore_init` is about to spawn, which the static model cannot see
start later in the same function. 18 on the crawler's module, between the
crawler thread writing its client per item and the worker threads setting
it up once per `lru_crawler` command under `lru_crawler_lock`. Nothing
actionable at memcached's rates; the same wall as before, frequency.
Phoenix: linear_regression's 17 definite unchanged; histogram 0/6,
kmeans 0/7, string_match 0/3 possible where there were 0/0, 0/1, 0/0,
not read; streamcluster 0/22 unchanged.

**The DPDK ring and stack, against their own annotations.** `rte_ring`
keeps its producer and consumer heads in two cache-line-aligned unions
with a 64-byte guard after each and after the header; the perf test runs
`enqueue_bulk` on one lcore and `dequeue_bulk` on another. The memzone
behind a ring or a stack is cache-line aligned, which the pass is told
with `heap-align=64`. With the headers as shipped: 0 definite, 88
possible, every pair inside one union (the producer's head beside its
tail, which the consumer reads: DPDK's deliberate layout), none across a
guard. With `__rte_cache_aligned` and `RTE_CACHE_GUARD` defined empty
and the name's `alignas` removed, so the unions sit at 64 and 88: 0
definite, 193 possible, 105 of them producer against consumer -- every
boundary the annotations draw, reported once they are gone. (At the
default heap alignment of 16 the header's `size`, `mask` and `capacity`
pair with the heads as well, 90 pairs, since the object may then start
anywhere in a line.) Possible rather than definite for two reasons the
remark states: the ring is one of two objects, and its allocation runs
more than once, one ring per sub-test; 48 threads, because the test's
entry is address-taken and every worker reaches both enqueue and dequeue
through a flag the walk does not read. `rte_stack` keeps its lock-free
lists, `used` and `free`, each cache-line aligned with a guard after it,
and the pile variant four such lists; the perf test pushes and pops from
every lcore. As shipped: 5 possible on the stack, each a list's head
beside its own length (`used` at 64 and 80, `free` at 192 and 208, the
pile's at 320 and 448), none across a guard. Stripped, with the lists
at 48, 80, 112 and 144: 10, among them `used`'s length against `free`'s
head and length and against the elements, and the pile's lists against
each other -- the pairs the guards exist for. Recall on these two
objects is two of two; mempool and distributor were not run. The test
harness's own `thread_args args[2]`, two 24-byte records on one line
with one lcore's `avg` beside the other's `s` and `sz`, comes out
possible in both variants, as it should. Set-up: DPDK 26.11.0-rc1
configured with meson for its generated header, the tests compiled by
ClangIR with the meson flags, in under a second each.

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
| `@stats_state` (56 B) | 49 | 17 | **no.** Every write after start-up is under `STATS_LOCK` (23 sites in 17 lock regions); the unlocked ones are `reserved_fds`, written once per worker from `memcached_thread_init` before the workers start, and the `memset` and `accepting_conns = true` in `stats_init`, before any thread. Only 21 of its 66 pairs involve `curr_items` or `curr_bytes`; the rest pair two fields each written rarely. The per-request writes are to `curr_items` and `curr_bytes`, by the workers on every link and unlink -- eviction too happens on the workers' allocation path, and the maintainer and crawler unlink only expired or flushed items, none with memtier's expiry of 0 -- all to the *same* two fields, which is true sharing that no layout removes. The fields on the other side of each pair are written per hash-table expansion, per crawl, per slab move, per connection, once per worker at start, per maxconns transition (`accepting_conns`), or -- `log_watchers` -- per pass of the logger's loop while a `watch` client is attached, which neither leg did. |
| `@stats` (224 B) | 78 | 89 | **no.** Every write after start-up is under `STATS_LOCK` (33 sites compiled in; two more under `TLS`, compiled out; the `memset` in `stats_init` runs before any thread). The per-request write, `total_items`, is the same field for every thread that links an item, and it never shares a line with `lru_maintainer_juggles` (offsets 0 and 104). Only 7 of the 167 pairs involve `total_items`; the rest pair two fields each written per slab move, per pass of the LRU maintainer's loop (`lru_maintainer_juggles`, every 1 ms to 1 s), per pass of the logger's loop, per crawl, per extstore compaction, per connection (`total_conns`), per maxconns event, per accepted connection in NAPI mode (`unexpected_napi_ids`, `round_robin_fallback`) or on a failure path. No rate of any of them was measured; an earlier version of this row quoted two, and neither was in the data. The one writer a workload could drive is the logger, which with a `watch` client attached writes `log_watchers` on every pass of its loop (every 1 ms to 1 s, and without sleeping while logs keep arriving) and with none attached blocks and writes nothing; neither leg attached one. |
| `@slab_rebal` (72 B) | 25 | 10 | **no.** A hand-off: `slabs_reassign` only try-locks `slabs_rebalance_lock`, which the rebalancer thread holds except while it waits, and the rebalancer reads `s_clsid`/`d_clsid` only after being signalled. With `slab_automove=2` the workers call it once per eviction -- more often, still a locked hand-off. No field is written per request. |
| `@active_crawler_mod` (56 B) | 3 | 7 | **no.** The same hand-off under `lru_crawler_lock`, once per crawl, whether a command or the maintainer's autocrawler starts it. |
| `@settings` | 0 | 729 | **no.** Read on every request; written by `SIGHUP`, by administrative commands (`cache_memlimit`, `verbosity`, `slabs automove`, `lru` tuning, `stats detail`, `extstore`, `lru_crawler sleep` and `tocrawl`), by thread start and stop (`lru_crawler`, `lru_maintainer_thread`), by `flush_all`, a client command that sets `oldest_live`, and by the LRU maintainer's extstore automove, which writes `ext_global_pool_min` once per window from a file the amalgamation leaves out but every measured binary links in, and only with extstore enabled, which neither leg was. A workload issuing `flush_all` continuously would make one pair here matter; none that measures a cache does. |

The counts in the table are from the passes as fixed in October: 155
definite and 852 possible on these five globals, of which 349 would have
been "cold" before the rule that a thread with accesses the walk cannot
place makes nothing cold. The reading covers every pair reported on a
global, the former cold ones included; the pairs the later heap-and-element
round adds -- one definite on `slabclass[0]`, 483 possible at elements the
analysis cannot tell apart, listed in the section on heap objects and
elements -- are not read here. So the first decision point of the plan,
which lives outside the repository, is reached for the pairs on globals:
none survives. It is not reached for memcached as a whole, and two things
the review found are outside what this reading covers:

- **One pair across objects.** `expanding` (`assoc.c:47`), read by every
  hash lookup, sits on the same line as `stats_state`, written on every
  item link: 0x455b8 against 0x45580 in the binary. That is false sharing
  between two objects. `--wtc-align-by-owner` puts them on two 64-byte
  lines but inside one 128-byte pair, which the adjacent-line prefetcher
  fetches together; the `hand` build separates `expanding` from
  `stats_state`'s per-request fields at both sizes (its 128-byte pair still
  holds `lru_crawler_running`, at offset 640 of the grown struct), and moves
  other things as well (`primary_hashtable` leaves `cas_id`'s pair), so
  neither arm below is a single-variable test of this one pair.
- **What this reading does not cover.** `lru_locks[]` holds 40-byte mutexes
  aligned to 16, so neighbouring slab classes share lines; `heads[]`,
  `tails[]` and `itemstats[]` are indexed per class and written under
  different locks; the item locks are a heap array. Since the heap-and-
  element round the analysis places `itemstats[]`, `slabclass[]`,
  `crawlers[]` and the heap `threads[]` at elements it cannot tell apart
  and reports possible pairs there (483, none read); `heads[]`, `tails[]`
  and `lru_locks[]` appear in no remark, and the summary still admits 4,377
  accesses through pointers it cannot follow.

## The measurements that test the review

Four builds of the same amalgamated memcached 1.6.29, each passing
memcached's own test suite (about 180,000 checks, run on a build of each arm
with `MEMCACHED_DEBUG`, which the suite needs; the one failure in every arm
is `t/line-lengths.t`, which reads a documentation file the copied tree does
not include):

- `orig`: `clang -O2`, as shipped.
- `hand`: every field in a definite pair given `__attribute__((aligned(64)))`
  -- the analysis' verdict applied as literally as possible: 50 attributes
  on 49 distinct fields in four objects. It was built from the verdict
  before the worker pool was visible; the later verdict adds two fields of
  `@stats` and one of `@slab_rebal`, 52 in all. This is maximal
  separation, not the least the verdicts need, and it moves `expanding` off
  `stats_state`'s line as a side effect, since `stats_state` grows to 704
  bytes.
- `cir`: the same source through ClangIR with no transformation: what the
  round trip itself costs.
- `ciralign`: through ClangIR with `--wtc-align-by-owner` (57 globals raised;
  the pass as fixed since raises 60: `bump_buf_tail`, `logger_count` and
  `logger_stack_tail` in, and after the October fixes `stderr`, a libc
  declaration it must not touch, out and `logger_gid`, written atomically,
  in).

The thresholds -- the review is overturned if `hand ÷ orig` is at least
1.02 with a 95% interval above 1.00 on any workload; the round trip is
acceptable if `cir ÷ orig` is within 2% -- were written down with the
results, not before the runs; an earlier version of this text said "fixed
in advance", and nothing on disk supports that. The next legs carry them
in the script.

The host: two Xeon Gold 6230 sockets, 80 threads, 64-byte lines, kernel
6.12, with nothing else of ours running. memcached (`-t 4`) was pinned to
CPUs 20-23, four physical cores of NUMA node 1, and memtier (eight threads,
25 connections each, pipeline 16) to CPUs 24-31, eight more such cores,
inside one container; their SMT siblings (60-63 and 64-71) were not given
to the benchmark, and whether anything ran on them was not recorded; ten
repetitions per arm, 20 s each, no warm-up, the first repetition kept. All
ten of memcached's threads -- main, the four workers, the LRU maintainer,
the crawler, the slab rebalancer, the logger, the hash-table maintainer --
shared those four CPUs, so the background threads time-shared the
workers' cores. The governor was
`powersave` with turbo on; fixing either needs root.

Two workloads per leg, each with set:get in two ratios. Leg 1 is memcached
as usually benchmarked: 2 GiB of memory and uniformly random keys, so
nothing is evicted. memtier gives every client the same random sequence,
so every client requested the same keys in the same order -- about
120,000 draws from 100,000 keys per client per run, which covers most of
the space -- a property of the tool and the same for every arm. Leg 2 answers the
objection that leg 1 leaves the background threads idle: 64 MiB of memory,
a separate key range per client, values of 32 to 4,096 bytes, and about 9
million evictions per 20-second write-heavy run (8.7 million on average
for 1:1, the same per operation in every arm), which keeps the LRU
maintainer running. Neither its rate nor the slab mover's was recorded:
that the maintainer was busy and the mover idle is inferred from the
eviction count and the uniform spread of sizes, not measured. The hit rate
was not recorded either.

Throughput relative to `orig`, median of ten, with a 95% bootstrap interval:

| | leg 1, 1:10 | leg 1, 1:1 | leg 2, 1:10 | leg 2, 1:1 |
|---|---|---|---|---|
| `orig`, operations per second | 1,227,592 | 1,388,929 | 1,126,722 | 910,297 |
| `hand ÷ orig` | 1.000 [0.989, 1.010] | 1.007 [0.994, 1.016] | 0.997 [0.969, 1.020] | 0.998 [0.981, 1.017] |
| `cir ÷ orig` | 0.974 [0.956, 0.987] | 0.987 [0.968, 0.993] | 0.974 [0.963, 0.993] | 0.966 [0.941, 0.990] |
| `ciralign ÷ cir` | 0.995 [0.979, 1.014] | 1.005 [0.996, 1.020] | 0.996 [0.981, 1.009] | 1.009 [0.967, 1.036] |

**The review stands, within what ten runs resolve.** Applying the
analysis' verdict -- every field it names put on a line of its own, and
`expanding` moved off `stats_state`'s line with them -- gives point
estimates within 0.7% of 1.00 on all four workloads, and the overturn
threshold, 1.02 on the point estimate, is not met on any. The intervals
admit anything from -3.2% to +2.0%; the leg-2 upper bounds cross 1.02 in
40-46% of bootstrap seeds taken one interval at a time, and one of the
four does in two seeds of three, so what the data say is "no loss of more
than about 3% and no gain of more than about 2%, with a 2% gain at the
edge of what the intervals admit", not "less than 1%". p99 latency was recorded and is in
the CSVs; the `cir` arms' p99 is 1-3% worse than `orig`'s, and no interval
was computed for it.

**The round trip through ClangIR costs 1.3% to 3.4% on the point
estimates**, which miss the 2% threshold on three of the four workloads;
by the intervals, which reach down to 0.941, no workload is shown within
2% and none is shown outside it. The `cir` binaries carry 555 text
functions against `orig`'s 400, so the round trip changes code generation
rather than adding a constant. It is a cost of the pipeline before it
changes anything, and it means a transformation applied through it has to
be measured against `cir`, not against `orig`. Measured so, raising 57
globals to a line each shows no change; the intervals reach +3.6%, so none
is excluded either.

What these two legs do not show: a gain on a workload neither of them
drives, such as continuous `flush_all` or a busy slab mover; anything about
the pairs at array and heap elements the analysis reports as possible and
this review did not read, or about the accesses it still cannot place; and
anything below about 2%, which ten repetitions on a host with frequency
scaling cannot resolve.

The arm order was not balanced. Leg 1 seeded its shuffle from the
repetition and workload names in a way that gave `get` and `set` the same
order, never ran `cir` last or `ciralign` third, and ran `ciralign` last
in six repetitions of ten and `hand` last in one. Throughput normalised
per arm and workload rises with position in leg 1 -- 0.992, 0.997, 1.002,
1.008 from first to last, a 1.6% drift (in leg 2 the positions span 0.6%
but not monotonically, +0.1% first to last) -- which favours
`ciralign` over `cir` by about 0.4% and works against `hand` by about 0.3%.
Leg 2 rotated the order, so every arm ran in every position, but always
after the same predecessor, and no arm's positions are balanced (`hand`
6/4/4/6, `orig` 4/4/6/6, `cir` 6/6/4/4, `ciralign` 4/6/6/4). The next
legs use a Latin square. The `hand` result is the same in both legs.

## What the analysis still cannot say

A `definite` verdict says two accounted-for threads repeatedly touch the
two fields; it does not say they do so *at the same time*. The walk has no
notion of `pthread_join`, so a thread started after another is joined is
still a second thread to it, and main's loops after it has joined its
workers still pair with them. (A pair between threads started in the two
arms of one `if` is no longer definite; a join is still invisible.)
memcached joins its threads only on a graceful shutdown, `SIGUSR1`;
`lru_crawler disable` stops the crawler without joining it, and
`lru_crawler enable` starts a new one -- a spawn, a stop and a second
spawn, which the walk also sees as two threads. So the limit arises there
too, on rare paths, not only in principle.

The rest of what a verdict leaves out, each the reason some pair above
does not cost what its verdict might suggest:

- **A lock.** Accesses under one mutex are not seen as serialised. On
  memcached the review found that a shared lock does not decide a pair by
  itself; each was decided by who writes it, by hand-offs, and by rate.
- **Rate.** Repeated or once is all the walk tells; every pair above was
  decided by how often one side runs, by hand.
- **Placement across objects**, which is the linker's and the allocator's.
  One cross-object pair was checked in the binary; nothing enumerates the
  rest, and the 128-byte pairs the adjacent-line prefetcher fetches
  together are not modelled at all.
- **Where an object starts.** "In *k* of 8 positions" treats the
  positions as equally likely; the real start is fixed by the linker and
  readable with `nm`.
- **Pointers it cannot follow**: 4,103 accesses on memcached, the slab
  allocator's items with their atomic reference counts and the state
  reached through `conn->thread` among them (set where the connection is
  made from a callback's `void *` argument, which is where the walk
  stops). Heap objects are placed by
  the call that allocated them and array elements by a constant index or
  the spawn loop's own; an element at any other index is placed on its
  array without knowing which element, and every pair there is at most
  possible (490 on memcached). A pointer loaded from a slot, a global or
  a field written in several places is one of the places the stores name,
  when every store can be placed and nothing writes the object another
  way, and a wrapper that returns one of several allocations is one of
  them; otherwise, and after a `realloc`, not followed. A member's
  address handed on is followed into the record; the address of a slot
  handed on as an out-parameter is not.
- **The optimiser.** The analysis runs on ClangIR before `-O2`, which may
  keep a store in a register, hoist it, or merge it.
- **Core placement.** Two threads on the two hardware threads of one core,
  or time-sharing one core, move no line between caches; in these legs the
  background threads time-shared the workers' cores.
- **The measured program is not quite the analysed one.**
  `slab_automove_extstore.c` is outside the amalgamation and inside every
  binary.

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
   written by different threads. Here the per-request fields are written by
   the worker pool and, under eviction, by the maintenance threads too, all
   to the same two fields, so the conclusion stands for a different reason.*
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
oracle the analysis is compared against -- an oracle with a measured
precision, not ground truth; see the top section.

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

17 codebases, 110,016 source files. *(The first recount of 16 October; the
September counts are in the second column. The second recount, with test
files set aside, gives 98,344 files and 1,623 sites -- dpdk 539, mysql 74,
compiler-rt 18, onetbb 23, abseil 5, prose 2,857; see the top section.)*

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

### Census, v5 (17 October, a-p13)

The corpus was fetched again on the second host with `tools/corpus-fetch.sh`
plus MySQL 8.0.39, SQLite 3.50.2 and upstream llvm-project at their
current heads, since the first corpus lives on a disk this round could
not use; linux, dpdk, folly, rocksdb and the others are therefore at
later commits than in v4, and compiler-rt and llvm come from upstream
rather than the fork the previous count read. The tool changes since v4
-- parameter brackets, `#if`/`#else` arms, string literals, padding types
by name, `test*` and `bench` directories -- went in at the same time, so
the move from 1,623 to 1,546 sites (98,439 files, 13,316 tests set aside)
mixes the two causes and is not a correction of v4: linux 836 to 798,
dpdk 539 to 491, compiler-rt 18 to 37 (upstream has `alignas(64)` where
the fork did not), rocksdb 24 to 14, `pad_field` 116 to 74. The shape
holds: 785 fields against 761 objects (51% / 49%), 703 of 785 field
annotations on one member (90%), 28 groups by begin-macro and 27 by
brace. The precision sample is not redrawn; the 74% [65%, 82%] of v4
stands as v4's figure.

### How the corpus synchronises (21 October, a-p13)

`tools/corpus-sync-census.py` reads the same files as the census above
(the same walk, the same tests set aside) and counts the lines that name
each synchronisation idiom: mutexes, condition variables, thread starts,
and the queues of the libraries a program would otherwise use. It counts
lines, not objects: an array of mutexes is one line, and a `std::mutex`
named in a `lock_guard<std::mutex>` is a line too. It answers which of
the analysis's inputs unported code holds most of.

| tree | files | tests | `pthread_mutex_t` | spinlock | `std::mutex` | `pthread_cond_t` | `std::condition_variable` | `pthread_create` | `std::thread` | tbb queue | moodycamel | folly queue | boost lockfree | DPDK ring | ck ring |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| abseil | 596 | 280 | 3 | 0 | 10 | 1 | 1 | 0 | 8 | 0 | 0 | 0 | 0 | 0 | 0 |
| dpdk | 4,914 | 393 | 98 | 3 | 0 | 8 | 0 | 8 | 0 | 0 | 0 | 0 | 0 | 81 | 0 |
| folly | 1,428 | 929 | 17 | 0 | 78 | 13 | 10 | 2 | 87 | 0 | 0 | 3 | 0 | 0 | 0 |
| jemalloc | 242 | 204 | 2 | 0 | 0 | 1 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| leveldb | 98 | 35 | 0 | 0 | 2 | 0 | 1 | 0 | 4 | 0 | 0 | 0 | 0 | 0 | 0 |
| libuv | 152 | 217 | 3 | 0 | 0 | 1 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| linux | 60,436 | 4,072 | 23 | 2 | 0 | 10 | 0 | 23 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| llvm-project | 28,124 | 45,290 | 68 | 26 | 2,690 | 57 | 51 | 21 | 96 | 0 | 0 | 0 | 0 | 0 | 0 |
| memcached | 96 | 0 | 46 | 0 | 0 | 12 | 0 | 15 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| mysql | 9,684 | 1,703 | 84 | 0 | 487 | 15 | 22 | 16 | 92 | 0 | 0 | 0 | 0 | 0 | 0 |
| nginx | 411 | 0 | 1 | 0 | 0 | 1 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| onetbb | 479 | 226 | 17 | 0 | 48 | 0 | 5 | 9 | 20 | 1 | 0 | 0 | 0 | 0 | 0 |
| parsec | 7,774 | 856 | 119 | 9 | 0 | 40 | 0 | 55 | 0 | 2 | 0 | 0 | 0 | 0 | 0 |
| postgres | 2,407 | 158 | 27 | 0 | 0 | 1 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| rocksdb | 1,135 | 272 | 5 | 0 | 256 | 3 | 22 | 2 | 18 | 0 | 0 | 0 | 0 | 0 | 0 |
| sqlite | 312 | 78 | 5 | 0 | 0 | 2 | 0 | 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| **all** | | | **518** | 40 | **3,571** | 165 | 112 | 161 | 325 | 3 | 0 | 3 | 0 | 81 | 0 |

At abseil 843b28efde9c, dpdk 9a45341f18a1, folly 514205c87be3, jemalloc
6390a79a935f, leveldb bb74ef739973, libuv a267f5688be2, linux
70dd4ee72eb2, llvm-project 83ee07799c34, memcached 2d51e364799b, mysql
d69a12a94538, nginx 1725db9059e8, onetbb af14bb56c642, parsec
2e492f771c67, postgres 26bd08fb5c06, rocksdb a3b0f187ff4b, sqlite
9d7c5df7f0e4.

Library queues are rare outside the libraries that define them: six lines
in all, folly's in folly and two of the three oneTBB ones in PARSEC.
DPDK's 81 ring creations are DPDK's own and choose single- or
multi-producer by flag at the call, which is the programmer stating the
topology. What the corpus is full of is mutexes and condition variables,
which is what hand-written queues such as PARSEC's are made of (a buffer
under a mutex, with a condition variable for each side), though this
count does not tell such a queue from any other use of a mutex. Either
way, recognising mutexes by name in unmodified code reaches further than
modelling the libraries' queues.

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

`memcached.h:431`, above `struct stats_state`:

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
| memcached, per file | 31 | 20 | 0 definite, 460 possible *(was 20 definite, 422 cold; 490 possible since the October fixes)* |
| memcached, amalgamated | 1 | 0 | 155 definite, 503 possible, 349 cold *(was 397 definite, 546 possible; since the October fixes 155 definite, 852 possible, 0 cold; since the heap-and-element round 156 definite, 1,335 possible)* |
| libuv | 44 | 18 | 11 possible *(was 32 scanned, 1 possible; the recount scans `src/` and `src/unix/` whole)* |
| leveldb | 28 | 13 | none *(was 11 scanned, 2 possible; the recount scans `db/`, `util/` and `table/` without tests; the 13 refusals were a missing platform define, see the top section)* |
| redis | 0 | 3 | needs its vendored dependencies built first |

libuv's refusals are its AIX, BSD and Darwin files, which is the scanner
being right. leveldb's two were `Slice`'s return slot, an escaping stack
object; since 16 October a stack slot in a function that runs more than once
gives no verdict at all, so they are gone. *(The scanned counts in this
table are not reproducible from the scan logs, which list 10 compiled and
20 refused files for memcached, 3 and 18 for libuv, 11 and 2 for leveldb
with the platform define; and leveldb's 13 refusals were a build artefact
-- with `LEVELDB_PLATFORM_POSIX` defined, 11 of 13 files compile and
report nothing.)*

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
largest shift, MySQL's, from fields towards objects, was mostly a
classifier bug: a `)` in `struct alignas(X) Name {` ended the record, and
every member of such a record counted as an object -- 23 sites in
`log0sys.h`; nine more fields come from sites the second census found.
Corrected, MySQL is 66 / 8 and the total 828 / 795; see the top section.)*

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

- **reordering fields by owner** reaches 44% *(51% after the second recount)*, and costs nothing when it
  works, where the hand-written version always costs a line of padding;
- **placing allocations by owner** reaches 56% *(49%)*, and is the half the language
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
object in every invocation. *(From the truncated walk. Recounted 16
October: 1,007 pairs on five globals, `@settings` among them with no
definite pair; after the heap-and-element round 1,491 pairs on 21 places,
the 484 new ones all but one at elements the analysis cannot tell apart;
see the top section.)* The four globals:

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
exists, `@settings` most of all at 0.09. *(Recounted: `@settings` has no
definite pair, so there is nothing of it to partition; `@slab_rebal` is
0.27 and `@stats` 0.31.)*

**This is the reason to measure before building.** The plan had reordering
as the flagship and padding as its fallback. Density says they are answers
to different objects, not better and worse answers to the same one, and
that an implementation which only reorders would leave the clearest case in
memcached untouched *(the retracted one, above)*. It also gives the transformation a cheap precondition
to check: compute the density, reorder below some threshold, pad above it,
and refuse when neither pays.

---

# What shape the transformation should take

Three measurements, taken before writing it, say what it has to be.

> **Withdrawn 16 October.** Recounted, 90% sit on one member and 10% mark a
> group, three ways: `__cacheline_group_begin_aligned` (28), an annotated
> closing brace (27) and an annotated opening brace (26). 165 files carry
> several, at most 23. People mostly mark boundaries, but grouping is not
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
made it the larger one at 56% *(49% after the second recount: about half)*. So it was built: `--wtc-align-by-owner`
raises the alignment of every global that one thread repeatedly writes while
a different thread repeatedly touches another, and counts the bytes.

On amalgamated memcached:

| | globals | padding |
|---|---:|---:|
| all mutable globals (1,303 of the 1,444 are constants) | 141 | 7,890 B |
| what the analysis picks | **73** | **4,254 B** |

*(Recounted 16 October, on the whole walk with the workers visible: **60**
globals, **3,489 B**. The argument below holds with the new numbers -- the
analysis picks 43% of the mutable globals and saves 4.3 KB against a flag, 7,778 B for the 139 defined ones --
and what the pass is worth when run is in the top section.)*

**The analysis selects half the mutable globals and saves 3.6 KB** *(43% and 4.3 KB, recounted)*. The
guarantee is identical either way: after the pass no two contended globals
share a line. A build flag that aligned all 141 *(139 defined; two were declarations)*
would give the same guarantee for 7.9 KB *(7.8 KB)* instead of 4.3 KB *(3.5 KB recounted)*, in a program that uses gigabytes.

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
| fields of one record (44%; 51% recounted) | **yes** | offsets are known at compile time, so the analysis says exactly which pairs share a line |
| separate globals (56%; 49% recounted) | no | placement is the linker's; a flag gives the same guarantee for twice the memory |
| fields of one heap object or array element | **yes**, since the heap-and-element round | the object is named by its allocating call and the element by its index or its thread; offsets within it are known as for a record |
| separate heap objects | unmeasured | placement is the allocator's; nothing but the compiler reaches it |

So the field case is the one where an analysis beats a flag, and after the
second recount it is the slightly larger share of practice (51%); it needs the record type rebuilt across
the module, which is the expensive machinery. An earlier draft had that
conclusion the other way round, on the strength of the alignment
attribute being cheap to write. Cheap to perform and worth performing are
different questions.
