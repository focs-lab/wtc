# Specialised implementations

Hand-written implementations that the compiler substitutes once it has proved
the corresponding fact about a program.

Each one is written and measured here *before* any pass learns to emit it.
That order matters: it establishes the ceiling. If a hand-written
single-producer, single-consumer ring is not meaningfully faster than the
general-purpose queue on a real workload, no amount of compiler work will make
it so, and it is better to find that out in week two.

Every implementation carries, in its header comment:

- the fact the compiler must prove before substituting it;
- what breaks if that fact is false;
- the measured numbers against the general-purpose baseline, and on what.

Nothing here yet.
