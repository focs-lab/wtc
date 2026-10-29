# The paper: topology and ownership of messages

The outline under the audit's thesis A: the compiler proves, for each
channel of a program, how many threads are on each side and how the
consumer owns the element, and from those two facts chooses the channel's
implementation and where the element lives. What each section argues,
which result carries it, then the experiments the outline needs and the
claims nothing measures yet. Numbers below are from `README.md`,
`docs/evaluation.md` and the leg notes under `benchmarks/results/`
(updated 28 October 2026 to the 26 and 27 October legs); figures from
untimed probes are marked "untimed" and are not the paper's.

## 1. Introduction

The claim: a cross-thread channel costs what the programmer declared it
to cost, and programs declare for the worst case, because the facts that
would allow less, the threads on each side and the life of the element,
are known to the program and to no library. A compiler that reads the
program can prove both and choose. Figure 1 (section 7, the ablation) is
the one-glance argument: with the element already in the cell, the
topology alone buys 8.4× and the pop form 2.1× on top, 18× together over
the program as written; against a queue that allocates per element the
step is 24×, and no allocator closes it.

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

Argue with the cost decomposition (`2026-10-28-eval-ablation`,
`2026-10-28-eval-w1`). A general-purpose queue that already holds the
element in its cell pays for its shared counters: the single-producer
ring through the shim is 6.6× it, and lending instead of handing out an
allocated `shared_ptr` another 1.7× (a loss at 256 bytes). A queue that
allocates every element on the heap pays the allocator as well: the full
step is 24×, and the best allocator (snmalloc) leaves the ring with the
element in its cell 5.9× ahead at 8 bytes. The allocator decides the heap
rows; nothing but the compiler can decide the others. Show what programs do: DPDK's rings with hand-set SP/SC
flags, folly's queues parametrised on both counts, spdlog's one
general-purpose queue whatever its pool size, Chromium's two reference
counts, and the bugs from getting it wrong (the record in
`docs/declared-topology-audit.md`). Carried by: the W1 mechanism pass
(allocations per element, cycles), the probe table in the plan, the audit
document's bug list.

## 3. The dialect and its contract

The `wtc` dialect: queue and mutex types, ten operations lifted from the
shim's calls (lock, unlock, push, pop, and three batch operations each for
the mutex and for pushes), each carrying the implementation chosen and answering what
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
pop borrows. Arrays of queues are one queue, every element together; threads on the
two arms of one branch count as one unless the function can return twice.
The reach table (RQ1, `2026-10-26-reach`: 43 queues, 31 exact, 10
conservative, 2 refused, none wrong; 18 of the 25 a reader calls
single-producer single-consumer proved so) and spdlog's verdict carry this
section; what stays inexact is explained by cause (a recursive scan,
threads started from another translation unit).

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
wrong verdict would break which lemma. Carried by: the lemma list, a deterministic test on the
interleaving each lemma relies on (`runtime/test/lemma_*`), the negative
tests (a ring forced on a two-producer queue is reported by
ThreadSanitizer and refused by `WTC_CHECK`). Testing the lemmas found two
defects the proofs had passed over, both fixed on 26 October: the
single-consumer queue's empty answer was not linearisable, and the queue
for one producer could make a consumer wait for another; the section
should say so, as the reason every lemma now has its test.

## 7. Evaluation

- **Figure 1, the ablation** (`2026-10-28-eval-ablation`, W10, on the
  current runtime): {MPMC, SPSC} × {element on the heap, in the cell
  handed out shared, in the cell moved out} × {8, 64, 256 bytes} ×
  {separate cores, SMT siblings}, under five allocators. Topology 8.4×,
  pop form 2.1×, both 18× (5.3–54); 9.0× over moodycamel by value; the
  element in the cell alone 7.7× on the ring, 1.3× on the segmented queue;
  the full diagonal against a heap-allocating queue 24× (26 Oct: 8.3×,
  2.0×, 17×, 8.8×, 24×).
- **Allocators as a factor** (every leg on the second host): glibc,
  jemalloc, tcmalloc, mimalloc, snmalloc, so that "use a better allocator"
  is answered in the same table.
- **The queue alone** (`2026-10-28-eval-w1`, on the current runtime):
  11× the program as written (5.1–27×), 0.84× rigtorp's ring (behind at
  8 bytes), 2.3× moodycamel's ReaderWriterQueue; the topology 6.6× and the
  borrow proof 1.7× of it; moving the element out about equals lending
  (0.96×) without a proof. On 26 Oct 10.2×, 5.9× and 1.7×: the program as
  written at 8 bytes under glibc is bimodal and fell in its fast mode more
  often then.
- **Latency under load** (`2026-10-27-eval-w2`): the ring has the
  shortest median at every rate (106–112 ns against 132–143 as written),
  its p99 below the program as written's at every rate and level with
  rigtorp's; the 26 October regression from 1 M/s and its fix
  (`2026-10-27-ring-gate-ab`) belong in a sentence.
- **A pipeline and its grain** (`2026-10-27-eval-w3`, `-w3grain`): the
  compiler's build is the hand-tagged one byte for byte, 1.90× the program
  as written, p99 0.27–0.29 µs level with the expert rings; the gain is a
  constant 0.04 µs a record, so the speed-up is (t + 0.04 µs)/t and above
  10% only below about 0.5 µs a stage: the honest statement of where this
  matters. Fusion (`2026-10-26-eval-fusion`) is the other end of the axis.
- **Many threads** (`2026-10-27-eval-w9`, `-pool`): the general-purpose
  queue 1.7× moodycamel and 1.6× LCRQ with separate producer and consumer
  threads; in the pairwise workload it trails LCRQ, LPRQ and the
  fetch-and-add array queue (0.69–0.84×) and is ahead of LSCQ (1.18×); the
  single-side substitutes 1.3× (many producers) and 4.6× (one producer, 14×
  at eight consumers); a thread pool 2.25× from the topology alone.
- **Applications**: spdlog read as written (`2026-10-28-eval-spdlog`, on
  the current runtime: 2.46× over spdlog as released, faster in all 36
  configurations; the topology's share 1.02×, 0.98× at one logging thread
  and 1.04× at eight, the runtime's queue 2.53×), ferret and dedup as
  the coarse-grained controls (within 2%, as the grain model predicts).
- **Reach and precision** (RQ1), **cost** (RQ5: 1.65× clang, under 0.4%
  code), **safety** (RQ6: every positive test and the three applications
  under ThreadSanitizer, `2026-10-28-tsan-apps`, no report from a
  substituted queue, ferret's one report PARSEC's own race; the planted
  violations; the declared-topology check's recall on wrong declarations
  made on purpose, `2026-10-26-declared-recall`).
- **One representation, two uses** (`2026-10-28-two-uses`): the facts
  that choose the implementation also check declared contracts (topology:
  substitution and the declared check; what may block: batching legality
  and the progress check, shared since 26 October). On the progress
  check they decide 3 of 58 verdicts, all in examples written to show
  it, and none of 10 in real code.
- **Batching and coarsening** (`eval-w7`, `eval-w8`): one paragraph, as
  known techniques with their crossover; legality is not profitability.

## 8. Related work

Point to `docs/related-work.md`: per-object choice by profile (Chameleon
and successors) versus by proof; confinement proofs for Java locks and for
reference counts, and why C++ has none; message memory crossing threads
(snmalloc, the exchange heap, pool allocation) and the queue ceilings
(FastForward, B-Queue, LCRQ/LPRQ); stream compilers; false sharing stated
as not novel; progress checks; raising to a dialect.

## 9. Threats to validity

One socket per leg (the timed host has one; the second host's legs run
within one socket, frequency not fixed, reporting paired ratios); the
shim's API (a program must use `wtc::queue`, or a locked queue class of
the shape `docs/lifting.md` lifts; the spdlog port replaces one
member, adds its bound and routes the pool's calls through a small
adapter); one translation unit; the port's departures from spdlog (bound,
spin); the grain model's reach (stages under about 0.5 µs); no cost model
for the transformations whose profitability is measured to cross over;
allocator and placement sensitivity, reported rather than hidden.

## Venue

With section 6 as a proof sketch and the ablation as figure 1, a venue
that values a correctness argument beside the measurements; without the
formal part, one that values systems results on real programs. The legs on the
strong general-purpose queue with allocators have run; the ones still to
repeat on the current runtime are listed below.

## Experiments the outline needs

| Section | Leg (`benchmarks/scripts/campaign.sh`) | State |
|---|---|---|
| 1, 7 figure 1 | `eval-ablation` | run 28 Oct on the current runtime (second host) |
| 2, 7 queue alone | `eval-w1` and its mechanism pass | run 28 Oct on the current runtime |
| 7 latency | `eval-w2` | run 27 Oct on the current runtime |
| 7 pipeline, grain | `eval-w3`, `eval-w3grain` | run 27 Oct on the current runtime |
| 7 many threads | `eval-w9`, `eval-pool` | run 27 Oct on the current runtime |
| 7 applications | `eval-spdlog` | run 28 Oct on the current runtime |
| 7 applications | `eval-ferret`, `eval-dedup` | run 21 Oct against the two-lock list; repeat |
| 4 reach | `reach.sh` | run 26 Oct with arrays, records and exclusive arms |
| 7 safety | `declared_recall.sh`, `tsan_apps.sh` | recall 26 Oct; TSan runs 28 Oct on the current runtime |
| 7 two uses | `two_uses.sh` | run 28 Oct on the current runtime and the shared blocking query |
| 7 cost | `compile_cost.sh` | run 21 Oct; repeat after the analysis changes |
| 7 batching | `eval-w7`, `eval-w8` | W7 26 Oct; W8 21 Oct, against the two-lock list |
| 9 cross-socket | the L8 leg | not run |

## Claims not yet supported by a measurement

- Not repeated on the current runtime (the single consumer's pop and the
  lock-free queue for one producer of 26 October, the ring's gated wait
  of 27 October): the queue on SMT siblings (W1smt, 21 October), fusion
  (its pipeline is the ring, built before the gate), batching (W7, the
  rings before the gate), PARSEC's ferret and dedup, the compile cost,
  and coarsening (W8).
- The profitability of coarsening and fusion has crossovers but no model;
  nothing in the compiler decides by cost yet.
- The declared-topology check has caught no wrong declaration that shipped.
  Of the three DPDK history fixes, the two mempools are now modelled and
  still missed: their threads are in another process or in other files.
  The third is a driver whose threads are the application's.
- No cross-socket placement has been measured on any host.
- The compile-cost figure predates the `std::thread`, array and shared
  blocking analyses.
