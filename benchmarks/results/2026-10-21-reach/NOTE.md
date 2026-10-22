# Reach and precision, 21 October 2026

`benchmarks/scripts/reach.sh` at 0eab2f0, with ferret and dedup built by
their `build.sh` at the same commit. (First recorded at 651528a and again
at 29bb531 with the same verdicts; since then the analysis says why it
counts many, and chooses a tag when the queue is built where a counted
loop starts a side's threads.) The verdicts are the analysis's, so the
machine does not matter; it ran in the evaluation's container on an
unreserved host.

35 queues: 21 exact, 4 conservative, 10 refused, **0 wrong**. Of the 22
queues a reader would call single-producer single-consumer, 15 are proved
so and specialised. Four more have their tag chosen when the queue is
built: the verdict is many on a side, and that side's threads are one per
iteration of a loop the program runs a known number of times.

What stands in the way, program by program:

- **ferret** (one thread per stage): four of five queues proved and held
  inline. The fifth is filled from inside the loader's recursive directory
  scan, and an access reached through recursion is not attributed to one
  queue. With the thread count from `argv`, every verdict but that one is
  exact (`mpmc`, `mpsc`): nothing is single-producer whatever the count.
  But each middle stage's threads come from a loop over that count, and
  the queues are built in `main` after it is read, where PARSEC
  initialises them, so the four queues after the loader get their tag
  then: inline rings when every loop runs once, which is what the build
  with one thread per stage gets at compile time.
- **dedup** as written: no verdict, because every queue is an element of
  an array indexed at run time. With the queues as globals, every queue is
  counted `mpmc`: the threads start in `Encode`, which only `main`, in
  another file, calls, so from this file `Encode` may run any number of
  times. With `main` in the same translation unit, every verdict is exact.
  dedup's two obstacles are thus arrays of queues and a program spread
  over several files. On the conservative rows the analysis's own account
  names the same cause as the oracle: the threads are started from
  `Encode`, which nothing in the translation unit calls.
- The examples and the compiler-built workloads: every queue exact.

Not yet in the table: the mutexes (the lock-batching remarks are in the
logs of `lock_loop`), and spdlog's asynchronous logger, whose queue, a
member of a pool reached through `shared_ptr` and `weak_ptr`, the analysis
does not identify.

Update, 23 Oct: the analysis now identifies it (`benchmarks/apps/spdlog`):
the queue is `q_ in any spdlog::details::thread_pool`, its producers many
(the logging threads come through the logger's virtual `sink_it_`), its
consumers one per iteration of the pool constructor's loop over the number
of workers, and the tag is chosen when the pool is built: the
single-consumer queue when that number is one, the general-purpose queue
otherwise. The table has not been rerun since; it will be with the next
reach leg.
