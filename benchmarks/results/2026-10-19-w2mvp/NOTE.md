The runs were made on the tree of commit e363247, with the binaries
built from it before the leg (`benchmarks/` with clang 18, march native).
`git_dirty` is true because the leg's own result directory was untracked
when it started; nothing else in the tree had changed.

The general-purpose queue's p99 at one element per microsecond spreads
from 6.3 to 44 microseconds across the five repetitions: it is close to
saturation at that rate, and its tail depends on how far behind the
consumer falls in each run. The specialised rows are tight.
