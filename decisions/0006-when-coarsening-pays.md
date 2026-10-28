# 6. When holding a mutex across critical sections pays

Taken 2026-10-28.

## Context

In a function marked `WTC_BATCH`, `--wtc-batch-locks` holds a mutex across
a loop's critical sections, letting it go every `WTC_LOCK_BATCH_K` (64)
sections, wherever the rules of `WTCBatchPushes.cpp` prove the loop can
wait for no other thread. Legal is not profitable. The code between two
sections runs with the mutex held once the loop is batched, so the other
threads wait through it. W8 (`benchmarks/results/2026-10-21-eval-w8`,
K = 64) found the batched loop 11 to 12 times faster with nothing between
the sections, and a loss once the work there grew: with two threads it won
at 100 ns (1.69x) and lost at 200 (0.61x); with four and eight it won at
200 ns (1.20x, 1.33x) and lost at 500 (0.35x, 0.59x). One thread never
lost. Until now every legal loop was batched.

## Decision

`--wtc-batch-locks` estimates the work between two sections and batches a
legal loop only below the crossover W8 measured. `WTC_BATCH` stays the
licence: the estimate can take a loop away from batching, never add one,
and the programmer still accepts the delay and the unfairness that
batching brings where it pays.

**The estimate** (`mlir/include/WTC/Transforms/WorkEstimate.h`) is one
iteration of the loop without its critical section -- its condition, its
step and the rest of its body -- counted in units over the CIR as the
front end emits it. Most operations are one unit (a load, a store, an
arithmetic or logical operation, a comparison); constants, slots,
addresses, casts and structure none; a division by a constant 2, by
anything else 40; floating division and square root 15; a transcendental
200; the time-stamp counter 90; the allocator 100, an allocation from the
thread's cache; memcpy and its kind with a constant length 2 plus one per
8 bytes. A call costs one and its callee's body. A branch counts its larger
arm and a switch every case, so the estimate is of the worst iteration. A
for loop whose trip count is a constant counts its iterations; any other
loop, a length not known here, recursion, a call into code not visible
here or an indirect call whose targets are not known has no bound, and an
estimate without a bound is over every threshold.

**Classes of host.** Both sides of the comparison are the host's: how
long a unit takes, and what handing the mutex over costs, which sets the
crossover. The model is therefore a table of classes of host, each with
one constant that turns units into nanoseconds and W8's thresholds there;
`--wtc-batch-locks=host=NAME` (`wtc-cc --batch-host NAME`) picks one, and
`ns-per-unit=X` or `threshold-ns=X` override a part. The constant is
measured on W8's own work, a dependent chain of xorshift-multiply rounds,
which the model counts at 25 units a round (the step's body 17, the call
1, the loop's load and store of x 2, its condition 2 and its step 3).

A threshold is on the axis of the estimate. W8's crossover is a level of
work: the speed-up of the batched loop over the plain one, its logarithm
interpolated linearly in the work between the two levels around the point
where it crosses 1 (from the medians of the leg). But the estimate of W8's
loop is more than its work: 212 units with no work at all, the loop's own
steps and, for about 190 of them, the two reads of the time-stamp counter
on the sampled sections, which run once in 61 sections and count on every
one. A threshold is therefore the crossover plus W8's estimate at no work
on that class, so that W8's loop is batched exactly at the levels of work
below its crossover.

| class (`host=`) | measured on | ns a round | ns a unit | W8 at no work | crossovers, 2 / 4 / 8 threads | thresholds, 2 / 4-8 threads | source |
|---|---|---|---|---|---|---|---|
| `sapphire-rapids` (default) | Xeon w9-3495X, 1.9 GHz fixed, turbo off | 3.71 | 0.148 | 31 ns | 152 / 245 / 305 ns | 180 / 275 ns | `2026-10-21-eval-w8`, timed |
| `cascade-lake` | Xeon Gold 6230, `powersave` with turbo | 3.06 | 0.122 | 26 ns | 177 / 48 / 47 ns | 200 / 70 ns | `2026-10-28-w8-cost`, untimed |

The thresholds are rounded down to 5 ns.

AMD's EPYC 9115 (Zen 5) has no class yet: W8 was not run there, the host
running another leg at the time.

**The threshold for a loop** is its class's for the number of threads
that take the mutex: two, or four to eight. One thread has no threshold:
no other waits for the mutex, and W8 at one thread never lost. A count
the analysis cannot give, and the counts W8 did not measure (three, more
than eight), take the lowest of the class's thresholds, which holds for
any number of threads W8 measured: 180 ns on the default class, 70 on
`cascade-lake`. The count comes from the thread walk (`ThreadInstances.h`)
when the mutex is a global: the threads that reach a lock of it, each
started once by `main`, a global constructor or a spawn the module sees,
with every lock they reach placed on a mutex the walk can name; a lock
taken before the first thread starts is not counted. Threads started in a
loop, a mutex that is not a global, or any lock the walk cannot place
leave the count unknown.

**What the compiler says.** Every legal loop gets a remark with the
estimate and the threshold, batched or not ("work between sections
estimated at 131 ns (887 units), under the 180 ns threshold that holds for
any number of threads"; "over the 180 ns threshold for the 2 threads that
take the mutex"; "not bounded (it runs a loop whose trip count is not
known here)", with a note at that loop). `cost=off` (`wtc-cc --batch-cost
off`) batches every legal loop, as before, and says nothing of the work;
the `eval-w8` flavours are built that way, since they measure the
transformation itself and W8 reads its work at run time.

**Validation** (`benchmarks/results/2026-10-28-w8-cost`, untimed, on the
Cascade Lake host, twelve paired repetitions on processors nothing else
held): W8 built with its work fixed at compile time (`-DW8_WORK_ROUNDS`)
at 0, 20, 50, 100, 200 and 500 ns, each three ways -- plain, batched
wherever legal (the model off), and the model's choice with the default
class's thresholds and that host's constant -- at 1, 2, 4 and 8 threads.
W8's threads are not counted, so 180 ns held: the model batched at 0 to
100 ns and refused at 200 and 500.

- Where it refused, it avoided the loss: model on over off 1.15 to 2.16
  above one thread. With one thread, which never loses, refusing cost 4
  to 5%.
- Where it batched, it kept the gain with one and two threads at every
  level (6.25, 3.66, 2.41 and 1.67 times the plain loop at 0 to 100 ns
  with two), and with four and eight at 0 and 20 ns (9.5 to 9.8; 3.83 and
  1.66).
- But on that host batching loses from about 50 ns with four and eight
  threads (0.93 and 0.95 at 50 ns, within the noise; 0.77 and 0.80 at
  100), where the timed leg's host had it winning up to 200 ns, and the
  default class batched there. Those are the crossovers the `cascade-lake`
  class records. With it the model batches W8 at 0 and 20 ns and refuses
  from 50 on, which on the same runs never batches at a loss and gives up
  2.41 and 1.67 at 50 and 100 ns with two threads: what the uncounted
  thread count costs there, since a count of two would get the two-thread
  threshold, 200 ns.
- On the timed leg's host, at that leg's rounds, the default class
  batches at 0 to 100 ns and refuses at 200 and 500, which by that leg's
  medians keeps every gain but the 1.20 and 1.33 at 200 ns with four and
  eight threads, and avoids every loss.

## What it costs

- **The worst iteration, not the usual one.** A branch counts its larger
  arm however rarely it is taken. That is kept: the estimate has no branch
  frequencies, and an arm that is short on average but long when taken
  still holds every waiting thread for its whole length each time. W8
  shows the price: it reads the time-stamp counter on one section in 61,
  and the estimate counts it on every one (about 190 of its 212 units with
  no work). Putting the thresholds on the estimate's axis absorbs that for
  W8's own loop, but it also means a loop without such a branch has W8's
  26-31 ns of overhead as extra room, and a loop with a heavier rare branch
  is refused sooner than its average would have it. A loop whose trip
  count is known only at run time is refused however short it turns out
  to be -- W8 as written is refused at every level, including no work at
  all, where batching wins 11 times.
- **Not counting the threads costs gains.** W8 starts its threads in a
  loop of a count read at run time, so the lowest threshold holds: on the
  timed leg's host the 1.2-1.3x at 200 ns with four and eight threads is
  given up, on the Cascade Lake host the 2.4x and 1.7x at 50 and 100 ns
  with two. Counting the threads of a spawn loop whose bound is a constant
  would help such programs; the thread walk counts any spawn in a loop as
  many.
- **One kernel calibrates every operation.** A unit's nanoseconds come
  from a dependent chain, where most of the loads and stores ClangIR
  emits for locals vanish in the optimiser. Code whose operations overlap
  takes less than its estimate, which only refuses more; code that misses
  the cache or takes a contended line takes more, and may be batched where
  it loses. The allocator is a fixed 100 units; a large block that maps
  memory, or a realloc that moves its block, is not seen.
- **The thresholds belong to a host, at K = 64.** The crossover is where
  the work outside the lock matches what handing the mutex over costs, and
  that differs between hosts by more than the work does: with four to
  eight threads 245-305 ns on one Xeon, 47-48 ns on another, under another
  governor, where the batched loop stayed fair and so handed the mutex to
  a waiting thread at nearly every release (a reading, not a measurement).
  The second host's crossovers are untimed and, at four and eight threads,
  somewhere between 20 and 50 ns. A program compiled for one class and run
  on another may be batched where it loses;
  a host without a class has no thresholds to trust until W8 has run on
  it. K = 8 was fairer and gained less, K = 512 gained more and was unfair
  already at 20 ns (`2026-10-21-eval-w8`).
- **Fairness is not in the model.** Where batching pays it still starves
  the waiting threads (Jain's index 0.59 at eight threads and 100 ns). That
  remains the programmer's call, made with `WTC_BATCH`.

## Batching pushes: no crossover to apply

`--wtc-batch-pushes` is unchanged. The model's question is whether a loop
loses throughput when batched, and W7 shows no such loss: batched over
plain is 1.00x at every K from 1 to 1024 on all three of the runtime's
queues (`2026-10-26-eval-w7`), and 1.8x on the retired two-lock list
(`2026-10-21-eval-w7`). W7 has no work between pushes to vary, and the
pushes are not held under anything the consumer waits for: a batch is
published every `WTC_BATCH_K` pushes and at the end, whatever the work.
What batching pushes costs is delay, up to `WTC_BATCH_K` times the work
between two pushes, and that is the licence `WTC_BATCH` gives, not a
profit the compiler can weigh. The same estimate could bound that delay in
the remark; that is left for when a queue whose push takes a lock comes
back.

## Fusion: the condition, without a pass

No pass fuses a pipeline's stages. W12 (`2026-10-26-eval-fusion`, three
equal stages on the EPYC 9115) found the fused loop faster below about
18 ns of work a stage (between 10 ns, 0.58x for the pipeline, and 20 ns,
1.12x) and 2.45-2.73x slower from 100 ns. A fusion pass would take the
same estimate of each stage's work per record, w1 ... ws, in the host's
nanoseconds, and fuse only when

    w1 + ... + ws  <=  max(w1, ..., ws) + H

since the pipeline runs at the rate of its slowest stage plus a hand-over
and the fused loop at the sum of the stages. H is fitted to W12's
crossover: three equal stages cross at 3w = w + H with w = 18 ns, so
H = 36 ns on that host, more than the 11.8 ns a record the pipeline's two
hand-overs cost with no work, because the fused loop overlaps the chains
of consecutive records. Until a pass exists and W12 has unequal stages,
the conservative reading is to fuse only stages of 10 ns of work or less
each. As with coarsening, those are levels of W12's work, and a pass
comparing estimates with them would first add what the estimate counts
in W12's stages beyond that work. The same limits hold as above, and one more: the pipeline
spends a core a stage, so fusing also gives cores back, which a
throughput threshold does not count.

## Options not taken

- **Expected rather than worst-case branches.** Counting each arm at half
  would let W8's timing branch weigh what it costs on average, and would
  batch a loop whose rare branch holds the mutex for a long time.
  Unsafe in the direction that loses.
- **Estimate after optimisation.** The LLVM IR the backend sees is closer
  to the machine, but the pass runs on the CIR, before lowering, where the
  critical section is still one operation pair.
- **Decide at run time.** The shim could let the mutex go once it has been
  held for a budget of time instead of after K sections, which needs no
  estimate and adapts to the host. It costs a time-stamp read per section,
  about the gain per section at small work, and changes the shim's
  contract; it stays a possible next step.
- **No model, only the licence.** Leaving the profitability to whoever
  writes `WTC_BATCH` is what the pass did. The programmer rarely knows the
  crossover, which depends on the host and on the contention, and W8 shows
  the loss is up to 2.9x.
