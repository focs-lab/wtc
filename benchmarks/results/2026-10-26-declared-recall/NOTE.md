# How many wrong topology declarations the check catches

The declared-topology check (`--queue-ownership-analysis`, part 3 of
`docs/declared-topology-audit.md`) found no broken declaration in the 33
it read, so it had no true positive on real code. This leg makes
declarations wrong on purpose and counts what the check says about each.
The question is recall: of the wrong declarations, how many stop the build.
Recall here is per audited ring, not per real bug.

## How

Untimed, verdicts only. Run on 26 Oct 2026 on the second server's NUMA
node 1, with the compiler built from this branch. The branch's merge base
with main is 881fe25, which includes PR #38: arrays of queues counted as
one queue, records copied from a literal followed, exclusive-arm threads
counted as one. The branch's first commit changes the check's report of a
ring's flags (see below), and nothing else under `mlir/` differs. The
same leg run before PR #38 merged is kept as `recall-before-pr38.tsv`;
what moved is listed below.

The run used the lab container `tsan-focs-sb:24.04`. Any Ubuntu 24.04 with
`clang-18`, `git`, `perl`, `python3` and `meson` serves; the container is
that plus packages other work needs. `tools.txt` records the compilers and
the two commits.

    benchmarks/scripts/declared_recall.sh <work dir> <dpdk checkout> \
      <supercollider checkout> <boost include dir>

Both checkouts are refused at any commit other than the audit's: DPDK at
6bbb7b3, supercollider at b2f6e4422088. Every program in the two sets below
is also checked unmodified, as a control.

### Mutants of the audited code

`mutants.tsv` holds 22 of them: 19 of DPDK's examples and 3 of
supercollider's `sndfile_backend`. They cover 9 programs and 13 queues
(simple_mp's two rings are mutated together) and 11 of part 3's 14 DPDK
rows. Left out are the two rows whose rings are bound by script, and
ip_pipeline. Its one mutant was dropped: putting a request back from the
data-plane thread either loops on the request or frees it while it is
still queued, and it never gives two producers at once.

Each mutant is one perl substitution that makes one declaration wrong.
There are three kinds:

- **a**: a side declared single that has two or more threads, by a flag or
  by an `sp`/`sc` call. 6 mutants.
- **b**: a second thread on a DPDK side declared single. 13 mutants.
- **c**: a second thread on supercollider's `boost::lockfree::spsc_queue`.
  3 mutants.

An added thread runs where nothing else does, because a launch on a busy
lcore fails with `-EBUSY`. Two ways:

- **New launches.** packet_ordering reserves the last worker but one, by
  ending the worker loop one lcore earlier. dma takes the next free lcore
  in its own sequence. efd_server and client_server_mp launch on the
  workers that sleep in the original.
- **Existing threads given a second role.** These start no new lcore:
  - distributor's first worker, or its rx lcore in the comment-invited
    swap;
  - qos_sched's lcore that no `--pfc` entry names, which the original
    leaves idle.

The last column of `mutants.tsv` says in which configuration two threads
reach the wrong side at once. No substitution adds or removes a line, so
every creation site keeps its position.

Each mutant is a hard-linked copy of the checkout with the one file
changed. It is run through `tools/declared-check/dpdk.sh` or
`supercollider.sh`, exactly as the audit ran them; `dpdk.sh` now takes
`EXAMPLES`, `DPDK_BUILD` and `ONLY` for this.

### The report of a ring's flags

The first run of this leg found the check misreporting a declaration. A
ring created `RING_F_SP_ENQ | RING_F_SC_DEQ` was reported "declared mpsc
(by its flags)" when no producer access was seen. That happened with
distributor's `dist_tx_ring` and with efd_server's and client_server_mp's
`rx_q`. The check applied a flag only to a side where it saw an access.

It now reports the flags as written, and says what qualifies them on a
side: "by its flags RING_F_SP_ENQ | RING_F_SC_DEQ; no producer access
seen". A side with no access seen is declared by its flags. Only a side
reached solely by `rte_ring_mp_*`/`mc_*` calls is many whatever the flags
say. The new test is `test/Transforms/DeclaredTopology/dpdk-ring-flags-as-written.c`.

Every warning counted below names the declaration as the program writes
it.

### Library programs

`tools/declared-check/recall_spsc.cpp` uses the real headers of three
libraries:

- `rigtorp::SPSCQueue` and `moodycamel::ReaderWriterQueue`, vendored under
  `benchmarks/third_party`;
- `boost::lockfree::spsc_queue`, from supercollider's vendored Boost.

Each library runs in seven shapes: a correct one, and six wrong ones. Four
start a second producer, on a function, on a lambda, with
`pthread_create`, and in a loop of two. One starts a second consumer, and
in one `main` pushes too. `folly::ProducerConsumerQueue` is modelled but
not on this machine, and was not downloaded.

Each program is checked, then built twice with `clang++-18 -O1
-fsanitize=thread`: with the libraries' assertions, and with `-DNDEBUG`.
Each build runs five times, 200,000 pushes per producer.

### Outcomes

- **caught:** an error. The build stops and names the threads.
- **warned:** a warning that names the declaration and says why it cannot
  be verified.
- **unresolved:** no line about the queue; only remarks that accesses to
  queues of its type cannot be resolved, which makes every such queue
  unknown.
- **verified:** the check says the declaration holds.
- **silent:** nothing at all.

`recall.tsv` holds every row, each control first (`orig:<id>`), with the
check's line.

## What it shows

**Audited code: 6 of 22 wrong declarations stop the build (27%). 13 of 22
change the check's verdict. None is verified.**

| kind | mutants | caught | warned | unresolved | verified |
|---|---|---|---|---|---|
| a, flag or call | 6 | 4 | 0 | 2 | 0 |
| b, second DPDK thread | 13 | 2 | 9 | 2 | 0 |
| c, second thread on supercollider's queue | 3 | 0 | 3 | 0 | 0 |
| all | 22 | 6 | 12 | 4 | 0 |

- **Controls.** Of the 13 queues: 6 verified, 2 warned and 5 unresolved;
  no control fails.
  - The 6 verified are packet_ordering's two rings, efd_server's and
    client_server_mp's `rx_q`, dma's `rx_to_tx_ring` and distributor's
    `rx_dist_ring`. efd_server's reads "declared spsc (by its flags
    RING_F_SP_ENQ | RING_F_SC_DEQ; no consumer access seen)".
  - The 2 warned are distributor's `dist_tx_ring` and supercollider's
    `write_frames`.
  - The 5 unresolved are qos_sched's two rings, bbdev_app's ring and
    simple_mp's two. Part 3 lists them as "cannot verify" or "no verdict".
    In the tool's output that is a remark at each unresolved access, not a
    line naming the ring.
- **Verdict changes (13).**
  - **Caught (6).** These are the six packet_ordering mutants, three on
    each ring. Example: `main.c:750:18: error: wtc: queue the heap object
    made at main.c:750:18 is declared spsc (by its flags RING_F_SP_ENQ |
    RING_F_SC_DEQ) but many threads pop from it`. The second consumer's
    error says 3 threads where at most 2 run; it counts both arms of the tx
    branch.
  - **Verified becomes warned (7).**
    - Six are the mutants of the per-node and per-port rings: efd_server's
      and client_server_mp's `rx_q` (4) and dma's `rx_to_tx_ring` (2). One
      allocation site there builds one ring per node or port in a loop.
      The verdict is the union over all of them, and with two producers or
      two consumers that union cannot separate "two threads on one ring"
      from "one thread on each".
    - The seventh is distributor's second `lcore_rx`, whose extra role is
      a conditional function pointer that the walk does not resolve.
- **No change (9).** The unmodified code already got the same warning or
  the same remarks:
  - distributor's `dist_tx_ring` (swap, second `lcore_tx`, 2): the
    threads that push to it are each started under a condition of their
    own, and whether the conditions can hold together is not known;
  - threads started through `std::bind`, or whose caller the module cannot
    account for (supercollider, 3);
  - an element of a per-lcore configuration array indexed at run time
    (qos_sched 2, bbdev_app 1);
  - a ring created or looked up by name according to the process's role
    (simple_mp, 1).

**What PR #38 moved.** The same leg, run before PR #38 merged
(`recall-before-pr38.tsv`), gave 6 caught, 11 warned, 5 unresolved and 10
verdict changes. PR #38 turned no miss into an error. It moved three
controls to verified, and with them three misses into verdict changes:

- **Exclusive arms counted as one.** packet_ordering's `workers_to_tx`
  control, warned before, is now verified; its three mutants were caught
  before and are caught now.
- **An array of queues counted as one.** dma's `rx_to_tx_ring` control is
  now verified. Its two mutants stay warned, which is now a change from
  verified.
- **Records copied from a literal followed.**
  - Distributor's `rx_dist_ring` control, unresolved before, is now
    verified, and its mutant (a second `lcore_rx`) went from unresolved to
    warned.
  - `dist_tx_ring` stays warned, now for the conditions under which its
    threads start rather than for an unidentified object.

Every remaining miss stops at a limit that part 3 names: an array indexed
at run time, an entry the walk does not resolve, one ring per object of a
loop, conditions it cannot relate, a ring found by name. None is caused by
a wrong count.

**Library programs: 18 of 18 wrong uses stop the build. The 3 controls are
verified.**

ThreadSanitizer, the same programs:

| build | runs with a race reported | programs with a race reported at least once | programs with any dynamic signal (a race or the library's assertion) |
|---|---|---|---|
| with assertions | 59 of 90 (60 and 62 in two earlier runs) | 14 of 18 (14 and 17 in two earlier runs) | 17 of 18 |
| `-DNDEBUG` | 85 of 90 | 17 of 18 | 17 of 18 |

Per library:

- **rigtorp.** A race is reported in every run of every shape, in both
  builds. With assertions, its `pop` assertion stops 22 of 30 runs.
- **moodycamel.** With assertions, a race is reported in 4 of 30 runs (5
  and 7 in two earlier runs, the 7 over all six programs instead of a
  few). Its debug
  re-entrance guard asserts in all 30, and that ends each run at a point
  that varies, so which programs get a report varies from run to run.
  With `-DNDEBUG` there is no guard, and a race is reported in 30 of 30.

  Under ThreadSanitizer its `atomicops.h` (lines 58-62) turns the fences
  into `AnnotateHappensBefore`/`After` on one global variable. That orders
  every fenced access against every other, which can hide races; here it
  did not hide them in the `NDEBUG` build.
- **Boost.** The four second-producer shapes and `main` pushing are
  reported in every run. A second consumer is reported in no run of either
  build, though it loses or doubles elements in every run.

  Two consumers that each read the slots and advance the read index with
  atomic operations make an atomicity violation, not a data race, and
  ThreadSanitizer reports only data races.

The check flags every program whose threads it can see, including the one
no dynamic tool flags.

## History

`history.tsv` gives the two searches of DPDK's commit messages. 49 distinct
candidates came out, and each one's verdict and reason are recorded. They
were run in the audit's checkout, a blobless clone: messages are local, and
reading a diff fetches its blobs. A search of the diffs themselves
(`git log -G`) needs a full clone and was not run, so a fix whose message
names no single-side declaration is missed.

Three are kept as real fixes of a wrong declaration. The check sees none
of them:

| commit | what was wrong | why the check does not see it |
|---|---|---|
| 5b2baff902 net/af_xdp (2020) | `buf_ring` created `RING_F_SP_ENQ \| RING_F_SC_DEQ`; the rx and tx paths on different threads enqueue and dequeue it | a driver: its rx and tx functions run on the application's lcores through the ethdev function table, and the file before the fix starts no thread (none of `rte_eal_remote_launch`, `pthread_create`, `rte_ctrl_thread_create`), so there is nothing to count. Not built: it needs libbpf's `xsk.h`, which is not installed here |
| 27a26d65f8 app/dumpcap (2023) | mbuf pool on the `ring_mp_sc` mempool driver, taken from by several queues | a mempool, not a ring: the analysis does not model mempools, and the file before the fix has no ring call or thread start |
| 46d3f57537 vhost (2020) | IOTLB mempool `MEMPOOL_F_SC_GET`, taken from by the control and the forwarding threads | a mempool again, in a library whose threads are the application's |

## How far it holds

- **The mutants are ours.** They are plausible edits: the comment-invited
  swap, an extra launch on a free lcore, an idle lcore given a role, a flag
  added. But they were chosen to cover the audited rings, not drawn from a
  distribution of real mistakes. Recall per ring, not per real bug.
- **One translation unit.** Every miss and every historical fix crosses
  what one translation unit shows: arrays configured at run time, a ring
  found by name in another process, a driver called by the application.
- **No dynamic baseline on DPDK.** ThreadSanitizer was not run on the DPDK
  mutants: they need the EAL, hugepages and ports. The library programs
  are the only side-by-side comparison.

## What it means for the claims

- **Errors only.** 6 of 22 wrong declarations in the audited code stop the
  build.
- **Verdict changes.** 13 of 22 change the check's verdict: 6 errors and 7
  verified-to-warned.
- **Never wrong when verified.** No wrong declaration is verified, and no
  control fails.
- **Library programs.** Where the threads are in sight: 18 of 18 caught,
  where ThreadSanitizer flags 14 to 17 depending on the run (17 without
  assertions).
- **Real bugs.** The record does not support "catches real bugs". The
  three historical fixes are out of the model: mempools and driver
  threads. Modelling `rte_mempool`, with its `SP_PUT`/`SC_GET` flags and
  ops names, would bring two of them in.
