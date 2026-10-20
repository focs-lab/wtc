# Compile cost, 21 October 2026

`benchmarks/scripts/compile_cost.sh` at 99ccccd, ten builds of each
source, medians, in the evaluation's container on an unreserved host
(no reservation: read the ratios, not the third digit). The `root` timer
was dropped from `runs.jsonl` and the summary regenerated after the run:
it is MLIR's enclosing timer, not a pass, and the script now leaves it
out.

`wtc-cc` costs 1.65 times what the same clang costs compiling the same
source the ordinary way on the compiler-built workloads (W3, W7, W8,
about 1,200 functions each with the standard headers), and 1.9 to 2.5
times on the small examples, where fixed costs weigh more. The largest
step is ClangIR's own: emitting CIR takes about 2.1 of the workloads'
4.8 seconds, `wtc-opt` about 1.0, translating back to LLVM IR 0.5, and
code generation 1.2. The specialised build's `.text` is 81 to 175 bytes
larger than the general build's: under 0.4% on the workloads (W3: 45,680
against 45,505 bytes), up to 1.7% on the smallest examples.

Before 99ccccd the ownership analysis took 10.9 of `wtc-opt`'s 11.6
seconds on each workload: whether a function's address is taken was
answered by walking the whole module once per function. It now takes
0.2 seconds on each workload and 0.03 on the examples. Of `wtc-opt`'s
wall time, 1.0 seconds on the workloads and 0.5 on the examples, MLIR's
timers account for 0.57 and 0.11; the rest is spent outside the passes
and the parser they time.
