# Benchmarks

Workloads representative of what the project targets, plus the harness that
measures them.

Measure the tail, not the mean. The whole motivation rests on p99 and p99.9
latency; an average can hide exactly the effect being chased.

Pin the processors a run owns and let nothing else run on them for its
duration, or the tail measures the neighbours rather than the code.

Baselines are the general-purpose libraries a developer would otherwise reach
for, and the hand-written specialised implementation from `runtime/`. The
second one is the ceiling: the compiler cannot beat code a human wrote for
this exact case, it can only reach it without the human.

Nothing here yet.
