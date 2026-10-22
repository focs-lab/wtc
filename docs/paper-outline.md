# The paper: topology and ownership of messages

The outline under the audit's thesis A: the compiler proves, for each
channel of a program, how many threads are on each side and how the
consumer owns the element, and from those two facts chooses the channel's
implementation and where the element lives. What each section argues,
which result carries it, then the experiments the outline needs and the
claims nothing measures yet. Numbers below are from `README.md`,
`docs/evaluation.md` and the leg notes under `benchmarks/results/`, or
from the untimed probes of 22 and 23 October recorded in the plan; the
latter are marked "untimed" and are not the paper's.

## 1. Introduction

The claim: a cross-thread channel costs what the programmer declared it
to cost, and programs declare for the worst case, because the facts that
would allow less, the threads on each side and the life of the element,
are known to the program and to no library. A compiler that reads the
program can prove both and choose. Figure 1 (section 7, the ablation) is
the one-glance argument: a single-producer ring alone or an element in
the cell alone each buy a few times, both together buy an order of
magnitude, and the best allocator does not close the gap.

Contributions: (i) a dialect that carries queues, threads and mutexes
through compilation with a written contract; (ii) a thread-instance
analysis that proves per-object topology, including through `std::thread`
and through pools whose size is read at run time; (iii) an ownership
analysis that proves the consumer only borrows; (iv) the transformations:
implementation by topology, element placement by ownership, the tag chosen
at construction, and a check of declared tags; (v) the evaluation, with an
ablation separating the two facts, five allocators, the lock-free
literature's queues, a grain sweep and an application read as written.

## 2. Motivation: where the cost is

Argue with the cost decomposition. Synchronisation on a one-producer
one-consumer channel costs about 5 to 10 ns an element (the inline ring:
9 ns at 64 bytes, untimed); memory allocated by the producer and freed by
the consumer costs 40 to 220 ns depending on the allocator (`shim_spsc`
4.5 Mops/s on glibc, 14.5 jemalloc, 40 snmalloc, untimed), so the
allocator decides the heap rows and nothing but the compiler can decide
the cell rows. Show what programs do: DPDK's rings with hand-set SP/SC
flags, folly's queues parametrised on both counts, spdlog's one
general-purpose queue whatever its pool size, Chromium's two reference
counts, and the bugs from getting it wrong (the record in
`docs/declared-topology-audit.md`). Carried by: the W1 mechanism pass
(allocations per element, cycles), the probe table in the plan, the audit
document's bug list.

## 3. The dialect and its contract

The `wtc` dialect: queue and mutex types, seven operations lifted from the
shim's calls, each carrying the implementation chosen and answering what
it guarantees about progress. The contract (`decisions/0003`): per-item
happens-before, linearisability per object, FIFO per producer, nothing
across objects, no timing. The contract is what makes "is this
substitution correct" a question with a written answer, and what the
runtime headers promise. Carried by: the dialect's op descriptions, the
verifier that checks each op against the call it lowers to.

## 4. The analyses

Thread instances by call string: a spawn in a loop is many, a function
whose address is taken is any thread, a counted spawn loop is recorded as
one thread per iteration, `std::thread`'s entry is read from the state
object its constructor builds, and a thread started through a vector's
`emplace_back` in a counted loop is still one per iteration. Queue
ownership: a queue is a global, a local, a member, a heap object, or a
member of an object the walk cannot place, named by record and offset
with the union of its objects' threads. Borrow analysis: the three ways a
consumer owns an element (transfer, share, borrow) and the proof that a
pop borrows. The reach table (RQ1: 35 queues, 21 exact, 4 conservative,
10 refused, none wrong, 4 chosen at run time) and spdlog's verdict carry
this section; the refusals are explained by cause (recursion, arrays of
queues, other translation units).

## 5. The transformations

The tag rewritten on the constructor; the element held in the cell on
every implementation with the pop deciding the ownership; `spsc_inline`
where the consumer borrows and the element is trivially destructible; the
tag computed at construction from a counted loop's bound
(`decisions/0005`); the declared tag checked against the proved facts,
with a build error where it is certainly broken. Batching and lock
coarsening are presented briefly as what they are (known techniques,
profitable only below a grain) and not as contributions. Carried by: the
pipeline example (hand-tagged build byte for byte equal to the compiler's,
RQ2), the ferret run-time tags, the declared-topology tests.

## 6. Correctness

Point to `docs/correctness.md`: each implementation is a contract queue
under its precondition; the precondition follows from the analyses'
over-approximation; the run-time tag's precondition follows from the loop
shape; what is not claimed (progress, destructor timing, ordering across
objects). The soundness obligations are listed so a reviewer sees where a
wrong verdict would break which lemma. Carried by: the lemma list, the
negative tests (a ring forced on a two-producer queue is reported by
ThreadSanitizer and refused by `WTC_CHECK`).

## 7. Evaluation

- **Figure 1, the ablation** (`eval-ablation`, W10): {MPMC, SPSC} ×
  {element on the heap, element in the cell} × {8, 64, 256 bytes} ×
  {separate cores, SMT siblings}, under five allocators. The diagonal is
  what the compiler does; each factor alone is a few times; together an
  order of magnitude (untimed: 1.6 to 4.7×, 1.4 to 3.4×, 5.7 to 19×).
- **Allocators as a factor** (every leg): glibc, jemalloc, tcmalloc,
  mimalloc, snmalloc, so that "use a better allocator" is answered in the
  same table (untimed: snmalloc lifts the heap SPSC row to 40 Mops/s, the
  cell row is 157 and does not move).
- **The queue alone** (`eval-w1`, `eval-w1smt`): the ladder of baselines
  per claim: the program as written, the best general-purpose libraries,
  the expert single-producer queues, the same source tagged by hand; the
  three ownership forms on the same queue (`bench-transfer`: transfer
  equals lending, five times sharing, untimed).
- **Latency under load** (`eval-w2`): the open-loop sweep; 542 ns p99 at
  10 µs against 2.1 µs for the best library and 5.5 µs as written.
- **A pipeline and its grain** (`eval-w3`, `eval-w3grain`): the gain is
  a constant 1.1 to 1.6 µs a record, so the speed-up is (t + 1.1 µs)/t:
  the honest statement of where this matters.
- **Many threads** (`eval-w9`): the general-purpose queue against
  moodycamel, oneTBB and the LPRQ family (LCRQ, LPRQ, LSCQ, FAAArrayQueue);
  the single-side substitutes beside them.
- **Applications**: spdlog read as written (the compiler's tag equals the
  hand tag; untimed, 1.6× over spdlog at two logging threads, 4.8× at
  four, p99 of the call 33 µs to 4.5 µs), ferret and dedup as the
  coarse-grained controls (within 1%, as the grain model predicts).
- **Reach and precision** (RQ1), **cost** (RQ5: 1.65× clang, under 0.4%
  code), **safety** (RQ6: every positive test and both applications under
  ThreadSanitizer; the mutation controls).
- **Batching and coarsening** (`eval-w7`, `eval-w8`): one paragraph, as
  known techniques with their crossover.

## 8. Related work

Point to `docs/related-work.md`: per-object choice by profile (Chameleon
and successors) versus by proof; confinement proofs for Java locks and for
reference counts, and why C++ has none; message memory crossing threads
(snmalloc, the exchange heap, pool allocation) and the queue ceilings
(FastForward, B-Queue, LCRQ/LPRQ); stream compilers; false sharing stated
as not novel; progress checks; raising to a dialect.

## 9. Threats to validity

One socket for the timed legs (the cross-socket leg needs the second
host's root); the shim's API (a program must use `wtc::queue`; the spdlog
port changes one member); one translation unit; the port's two departures
from spdlog (bound, spin); the grain model's reach (stages under 10 µs);
the ring's two regimes before the slip and the distributions reported
after it; allocator and placement sensitivity, reported rather than hidden.

## Venue

With section 6 as a proof sketch and the ablation as figure 1, a venue
that values a correctness argument beside the measurements; without the
formal part, one that values systems results on real programs. The paper
should not be sent before the legs below are rerun on the strong
general-purpose queue with allocators, because every ratio over "the
program as written" changes with it.

## Experiments the outline needs

| Section | Leg (`benchmarks/scripts/campaign.sh`) | State |
|---|---|---|
| 1, 7 figure 1 | `eval-ablation` | does not exist as a timed leg; untimed probe only |
| 2 | `eval-w1` mechanism pass | run 21 Oct with the old B0; rerun with allocators |
| 7 queue alone | `eval-w1`, `eval-w1smt` | run 21 Oct; rerun: new B0, by-value storage, slip, allocators, transfer variants |
| 7 latency | `eval-w2` | run 21 Oct; rerun (same reasons) |
| 7 pipeline, grain | `eval-w3`, `eval-w3grain` | run 21 Oct; rerun with the new B0 |
| 7 many threads | `eval-w9` | run 21 Oct with the old generic; rerun with the segmented queue and the LPRQ variants |
| 7 applications | `eval-spdlog` | does not exist as a timed leg; untimed smoke only |
| 7 applications | `eval-ferret`, `eval-dedup` | run 21 Oct; rerun with the run-time tag and the new B0 |
| 4 reach | `reach.sh` | run 21 Oct; rerun to add spdlog and the std::thread tests |
| 7 cost | `compile_cost.sh` | run 21 Oct; rerun after the analysis changes |
| 7 batching | `eval-w7`, `eval-w8` | run 21 Oct; valid as is |
| 9 cross-socket | the L8 leg | needs the second host's `bench-env`; not run |

Every timed leg needs the whole host quiet and fixed frequency
(`check_env.sh`); the first host is held by another lane's legs, the
second has no root yet.

## Claims not yet supported by a measurement

- The ablation's "order of magnitude together" is untimed and on one
  host; figure 1 does not exist in a committed leg.
- Every speed-up over "the program as written" in the README is against
  the two-lock list, not the segmented queue; the honest ratios will be
  smaller and are not yet measured.
- spdlog's 1.6× and 4.8× are untimed, on glibc, with one worker; the
  allocator and payload sweeps have not run.
- "Level with the expert ring at 8 bytes" is untimed (ring 429 against
  rigtorp 400); the timed W1 of 21 Oct says 8%.
- The transfer form's "equal to lending" is untimed.
- The segmented queue's single-consumer slip has no timed measurement, and
  its many-consumer consumer polls the producers' counter when empty,
  which the transfer variants showed to cost (untimed, 19 against 28
  Mops/s); not yet understood.
- No cross-socket placement has been measured on any host.
- The compile-cost figure predates the `std::thread` and abstract-key
  analyses.
- The grain model's constant was measured with the old general-purpose
  queue; the crossover moves with the new one.
