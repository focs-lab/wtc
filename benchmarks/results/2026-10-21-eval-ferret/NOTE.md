# eval-ferret: PARSEC ferret, 21 October 2026

`campaign.sh eval-ferret` at 915a601 on focs, eighteen cores with their
SMT siblings offline (processors 4 to 21), governor performance, turbo
off, 10 repetitions, PARSEC's simlarge input (taken from the published
image of the PARSEC tree and checked against its SHA-256). Builds: PARSEC's
own (`orig`, condition variables and its thread pool), the port as written
(`generic`, every queue general-purpose), the compiler's (`compiler`), and
with one thread per stage also hand-written inline tags (`hand-one`).
The middle stages run 1, 2 or 4 threads each. Time is ferret's own query
time. It ran 18:21 to 18:27 local; the controls (shim_generic, 64 bytes)
read 1.07 to 1.16 Mops/s at the start and 1.10 to 1.22 at the end.

All 100 runs computed PARSEC's output: byte for byte at one thread a stage,
line for line above, where the order of the results varies.

| threads a stage | orig | generic | compiler | hand-one |
|---|---|---|---|---|
| 1 | 3.45 s | 3.47 | 3.47 | 3.45 |
| 2 | 1.74 | 1.75 | 1.74 | |
| 4 | 0.89 | 0.89 | 0.89 | |

Every build is within 1% of PARSEC's own, at every thread count. With one
thread a stage the compiler holds four of ferret's five queues inline, and
it changes nothing measurable: a stage spends milliseconds on each query
(segmenting, extracting and ranking an image), against which the 1.1 µs a
record that eval-w3grain attributes to the general-purpose queues is a
few parts in ten thousand. The port's spin-waiting on `try_pop` costs
nothing either against PARSEC's condition variables.

So ferret answers the reach question (RQ1: four of five queues proved at
one thread a stage) and the safety question (the compiler's build computes
PARSEC's output, and runs clean under ThreadSanitizer in
2026-10-21-tsan-apps), but not the speed question: it is too coarse-grained
for its queues to matter.
