# DPDK's mempool fixes under the declared-topology check

**Neither of DPDK's two historical mempool bugs is caught or warned about,
at the parent of its fix or at the fix. The audit's DPDK driver and the
recall study, rerun, give the same outcome on every row.**

The recall study (`../2026-10-26-declared-recall`, `history.tsv` there)
found three real fixes of a wrong single-side declaration in DPDK's
history, none visible to the check. Two of them were mempools, which the
check did not model then. It does now ("Mempools" in part 3 of
`docs/declared-topology-audit.md`). This leg:

- runs the check on the files of both fixes, at the parent and at the fix;
- reruns the audit's DPDK driver and the recall study, with main's
  analysis and with this branch's, to see that nothing else moved.

## How

Untimed, verdicts only. Run on 28 Oct 2026 on the second server's NUMA
node 1, in the lab container `tsan-focs-sb:24.04`, with the compiler built
from this branch. `tools.txt` records the compilers and the commits.

    tools/declared-check/dpdk-history.sh <work dir> <dpdk clone> <libpcap include dir>

- **DPDK.** A blobless clone made on 28 Oct. Each revision's tree is
  exported from it with `git archive`.
- **Configuration.** meson configured the two parents (23.11.0-rc2 and
  20.11.0-rc0) for the generated `rte_build_config.h` only. It ran outside
  the container, whose image has no meson, and the script was handed the
  result (`BUILD_DUMPCAP`, `BUILD_VHOST`).
- **libpcap.** dumpcap includes `pcap/pcap.h`. The headers of libpcap
  1.10.4 were used, the version Ubuntu 24.04 ships.
- **Translation units.** Each file is one, as DPDK's build makes it, and
  is compiled as `tools/declared-check/dpdk.sh` compiles the examples. For
  dumpcap, `lib/pdump/rte_pdump.c` is checked too: it is the file where
  the gets that break the declaration are made, in the other process.

`history.txt` is the script's output.

## Outcomes

| fix | file | at the parent | at the fix | outcome |
|---|---|---|---|---|
| 27a26d65f8 app/dumpcap: ops `"ring_mp_sc"` to `"ring_mp_mc"` | `app/dumpcap/main.c` | verified in this translation unit: `declared mpsc (by its ops "ring_mp_sc"; no consumer access seen), verified in this translation unit; rte_mempool_lookup hands pools to code that finds them by name` | no declaration line: `"ring_mp_mc"` declares nothing single | missed |
| | `lib/pdump/rte_pdump.c` | unresolved: the pool's get is a "queue access is unresolvable" remark | the same | missed |
| 46d3f57537 vhost: `MEMPOOL_F_SC_GET` dropped from the IOTLB pool | `lib/librte_vhost/iotlb.c` | unresolved: 10 accesses "could not be identified", and no line names the pool | the same | missed |

**At the parents: caught 0, warned 0, missed 2.** At the fixes, dumpcap
declares nothing single, and nothing is left to check. vhost's remaining
`MEMPOOL_F_SP_PUT` is still wrong (below), and it is unresolved too.

### dumpcap

- **What breaks the declaration.** dumpcap hands its pool to
  `rte_pdump_enable_bpf` (`main.c:811`), and the primary process takes from
  it. pdump's `pdump_copy` runs as a receive or transmit callback on
  whichever lcore polls the captured queue. It allocates once per packet:
  `rte_pktmbuf_copy` at `rte_pdump.c:127`, or `rte_pcapng_copy` at `:123`.
  With several queues captured, several lcores take from a pool declared
  single-consumer.
- **What the check sees in dumpcap.** No get at all. A declared side with
  no access holds in this translation unit vacuously, and the line says
  both: "no consumer access seen", and `rte_mempool_lookup` as a reason
  the pool is used elsewhere. The only puts are dumpcap's frees of the
  mbufs it takes off the capture ring (`main.c:930`). Their pool is not
  named, and they all run on one thread.
- **What the check sees in `rte_pdump.c`.** The pool is `cbs->mp`, a
  field filled from the request the secondary process sends
  (`rte_pdump.c:193`, from `p->mp` at `:325`). The walk cannot place it,
  so the get is unresolved. The callbacks are also reached through
  `rte_eth_add_first_rx_callback`, by the ethdev library, not by a thread
  the file starts.
- **A run-time guard missed it too.** `pdump_validate_ring_mp`
  (`rte_pdump.c:456-470` at the parent) refuses a pool whose flags say
  `RTE_MEMPOOL_F_SP_PUT` or `RTE_MEMPOOL_F_SC_GET`. dumpcap declared by
  ops name, which leaves the flags clear, so the guard let it through.
  The check reads both spellings, but the threads are in the other
  program.

### vhost

- **What breaks the declaration.** Two threads take from the pool:
  - the control thread, in `vhost_user_iotlb_cache_insert` on an IOTLB
    update from the vhost-user handler (`vhost_user.c`);
  - a forwarding lcore, through `vhost_user_iotlb_pending_insert` on a
    translation miss (`vhost.c:58`).
- **What the check sees.** `iotlb.c` starts no thread. Its functions are
  called by those two threads from other files, so each is a root that
  nothing in the file calls.
- **The pool is never placed.** It is kept in `vq->iotlb_pool`, written
  twice in `vhost_user_iotlb_init`: first with what `rte_mempool_lookup`
  finds under the pool's name, which is freed at once, then with
  `rte_mempool_create`'s result. A load of the field may be either, and
  the first is no object the walk can place. So every access is
  unresolved, and a pool with no access gets no line.
- **Either cause is enough.**
  `test/Transforms/DeclaredTopology/dpdk-mempool-iotlb-shape.c` is a
  reduced copy with both threads in sight. With the field written once,
  the check stops the build. With vhost's lookup, free and create, it is
  unresolved.
- **After the fix.** `MEMPOOL_F_SP_PUT` stays, though both threads still
  put. The forwarding lcore puts when the pool is empty
  (`pending_insert` evicts) and when a miss request fails (`vhost.c:63`).
  The control thread puts when it removes entries or meets a duplicate.
  These puts are under two different locks, the pending list's and the
  cache's. The check is silent on this too. DPDK replaced the mempool in
  2022 (f8f6b1c174, "vhost: stop using mempool for IOTLB cache").

## Nothing else moved

- **lit.** 317 tests pass: 309, and eight new under
  `test/Transforms/DeclaredTopology/dpdk-mempool-*.c`. Cases were added to
  `dpdk-ring-unplaced.c` and `BatchLocks/shared-facts.cpp`.
- **The audit's DPDK driver.** `tools/declared-check/dpdk.sh` was run at
  6bbb7b3 with main's analysis (9cd9bb8) and with this branch's
  (`driver-main.txt`, `driver-mempool.txt`). Every line of the first is
  in the second, unchanged. The 23 new lines are all about mempools:
  - 8 verdicts on the examples' mbuf pools, with 7 lines saying why a
    side is many;
  - 8 remarks on pool accesses that cannot be placed: bbdev_app's
    operation pools, reached through `rte_bbdev_op.h`, and simple_mp's
    pool, found by name.

  No example declares a mempool side single. A first run also warned that
  bbdev_app's two mbuf pools "cannot be checked", since their type has an
  access that cannot be placed. Those pools declare nothing, so there is
  nothing to check, and the branch now gives such a queue no warning,
  pool or ring.
- **The recall study.** `benchmarks/scripts/declared_recall.sh` was run at
  the audit's commits. `recall-main.tsv` holds the mutants under main's
  analysis; `recall.tsv` holds the mutants and the library programs under
  this branch's.
  - **Mutants.** All 44 rows have the same outcome under both, and the
    same as 26 Oct's: 6 caught, 12 warned, 4 unresolved, none verified.
    The 13 controls are 6 verified, 2 warned and 5 unresolved.
  - **Two quoted lines differ.** bbdev_app's and simple_mp's unresolved
    rows quote the first "unresolvable" remark in the log. That is now a
    remark on a mempool access (`main.c:715:6`, `mp_commands.c:18:6`).
    The rings' own remarks (`main.c:807:12`, `mp_commands.c:21:6`) are
    still in the logs.
  - **Library programs.** All 18 wrong uses are caught and the 3 controls
    verified, as on 26 Oct. ThreadSanitizer reported a race in 58 of 90
    runs with the libraries' assertions (14 of 18 programs), and in 85 of
    90 without them (17 of 18). That is within 26 Oct's range.

No false "verified", and no false error on a control.

## How far it holds

- **Two fixes, both out of reach.** In a reduced copy with the threads in
  sight, the model's reading is right: the iotlb-shape test stops the
  build. Neither real fix has its threads in sight. The record still does
  not support "catches real bugs".
- **"Verified in this translation unit" on dumpcap is only its
  qualifier.**
  - A declared side with no access in the translation unit is verified
    vacuously. The line says so ("no consumer access seen") and names the
    reason the pool may be used elsewhere; a reader who takes "verified"
    without the qualifier is misled.
  - Making such a side a warning when a reason is named would turn dumpcap
    into "warned".
  - It would also turn two of the recall's controls from verified into
    warned: efd_server's and client_server_mp's `rx_q`, whose consumer is
    another process. That is not done here; it is a choice for the
    check's next revision.
  - **Prepared, off; on since 29 Oct.** The option `vacuous-outcome`
    (`VACUOUS=1` in the scripts) reported such a verdict as "verified
    (vacuous: no consumer access seen)", and the recall driver counts it
    as vacuous. Since 29 Oct it is the default, and the verdict no longer
    says "verified" at all: dumpcap's line reads "declared mpsc (by its
    ops "ring_mp_sc"; no consumer access seen), vacuously satisfied: no
    consumer access in this translation unit; rte_mempool_lookup hands
    pools to code that finds them by name" (`vacuous-outcome=false`, or
    `VACUOUS=0`, gives the wording recorded here). The lines in this
    directory were recorded before that. Run with the option, this leg
    changed 3 verdicts and no other line:
    - in the driver, the two `rx_q` rings (efd_server `init.c:188`,
      mp_server `init.c:168`);
    - in the recall study, the 4 control rows of those two rings;
    - in the history, dumpcap at the parent.

    No mutant row moves.
- **One translation unit, as before.** `rte_pcapng_copy`, which takes
  from the pool it is handed, is not modelled. It has no body here, and
  handing it the pool is named as a reason the pool is used out of sight.
- **The rerun is untimed.** The ThreadSanitizer columns of `recall.tsv`
  vary from run to run, as they did on 26 Oct.

## What it means for the claims

- **Real bugs.** The check has caught no wrong declaration that shipped.
  Two of DPDK's three history fixes are now in its model, and both are
  missed for reach, not for the model:
  - the threads that break them are in another process (dumpcap) or in
    other files (vhost);
  - vhost's pool is also stored in a way the walk cannot place.
- **No regression.** The audited rings and the library programs keep
  every verdict.
