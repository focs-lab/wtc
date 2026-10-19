The two rings that hold elements inline (ring_inline, ring_borrowed) and
the shim's spsc_inline tag are bimodal in this leg: most runs land in the
ring's slow regime, where the consumer reads right behind the producer and
every line it reads is still being written (runtime/spsc_ring.hpp), and a
few in the fast one (ring_inline at 64 bytes: median 14.6, fastest 149
million a second; September's leg measured 148). Five repetitions do not
settle a median between two regimes, so the rows marked with a warning
are reported with their ranges and not as single ratios.

The runs were made on the tree of the commit that adds these results'
documentation, recorded in `meta.json` under its sha from before the
branch was rebased onto `main` (e6581d1); the rebase changed no tree.
