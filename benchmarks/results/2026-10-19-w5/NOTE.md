The control this leg exists for: the shim with the generic tag, whose
constructor now chooses among three implementations, against the gate's
legs. It ran at 0.93 million elements a second (0.85 to 1.06), where the
September legs measured 0.94 and 0.95: the third arm of the tag costs
nothing measurable.

orig_generic, the shim as it was before the tag, verbatim and unchanged
since September, came out lower than in either September leg: 0.69
(0.59 to 0.89) against 0.88 and 0.96, below shim_generic in four of five
repetitions. Its code is the same; the binary was rebuilt for this leg.
This was not investigated, and it is not read as the tag making the
shim faster: two producers and two consumers on a mutex queue is the
noisiest configuration measured here (every row carries a warning).

The runs were made on the tree of commit e363247; `git_dirty` as in the
w2mvp leg of the same session.
