# The ring's latency gate, paired against main (27 Oct)

What it tests: whether gating the consumer's slip on a streaming producer
(runtime/stream_gate.hpp) removes the latency regression the 26 Oct W2 and
W3 legs showed for the single-producer ring, without costing throughput or
the tail of a paced stream.

Setup: the two-socket AMD host, node 1, cores pinned per run, frequency not
fixed (paired unpinned protocol). OLD is main before the gate, NEW is the
gate with its probe backing off exponentially (256 to 65536 catch-ups).
Both sides run the same configuration in the same round, interleaved, ten
rounds. Ratios are NEW/OLD within a round, the median per configuration,
then the geometric mean with the range of the medians (`ab.py runs.jsonl`).
Rows: W1 throughput at 8/64/256 B, W2 latency at 300 ns to 10 µs between
pushes, W3 pipeline at 0.5 to 4 M records/s; the general queue
(`shim_generic`) and rigtorp's ring are controls the gate does not touch.

Results (NEW/OLD):

- W3, share of records late by 0.9 µs or more, `shim_spsc_inline` and
  `shim_spsc`: at 1, 2 and 4 M/s from 46-92% to 0.3-0.4%; at 0.5 M/s
  0.82-0.84 of an already small share (0.3%).
- W3 p99: at 1-4 M/s 0.18-0.29× (1.1-1.7 µs down to 0.29-0.33 µs);
  at 0.5 M/s 1.01-1.02×. p50 at 1-4 M/s 0.17-0.42×, at 0.5 M/s 1.02×.
- W2 p50 for the ring variants: at 300 and 500 ns 0.13-0.22× (590-825 ns
  down to 104-136 ns); at 1, 2 and 10 µs 0.95-1.06×. p99 at 1 µs 0.14-0.16×
  (1.13-1.19 µs down to 0.15-0.19 µs).
- W1 throughput: geometric means 0.99-1.01× for every ring variant and
  `shim_mpsc`; single cells 0.96-1.04×.
- Controls: `shim_generic` 0.95-1.04× everywhere; rigtorp's ring 1.00× in
  W1 (single cells 0.89-1.18×), 1.02× in W3, 1.04-1.13× in W2 p50: host
  noise of the size the gated variants show at 1-10 µs.

`runs-probe256.jsonl` is the first A/B (26 Oct, OLD = main of that day),
with the probe at a fixed 1 in 256 catch-ups: it fixed the same regression
but raised W3's p99 at 0.5 M/s 3.7-4.1× in all ten rounds (290-330 ns to
0.8-1.25 µs), because on this host the probe delayed about 1% of records.
The exponential back-off is the answer to that run.

How far it holds: one host, frequency not fixed, untimed against the host's
other work; the paired design cancels what affects both sides of a round.
The 26 Oct eval-w2 and eval-w3 legs predate the gate and should be rerun
before their ring numbers are quoted as current.
