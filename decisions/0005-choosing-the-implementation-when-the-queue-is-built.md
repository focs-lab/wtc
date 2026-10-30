# 5. Choosing a queue's implementation when the queue is built

Taken 2026-10-21.

## Context

The analysis counts a side's threads as one or many, and a spawn in a loop
is many: that is the count which holds whatever the loop does. Programs
that size their stages or pools at run time -- PARSEC ferret's threads per
stage from its command line, a thread pool built with a given number of
workers -- therefore keep the general-purpose queue even when they run
with one thread a side, which is how they are often run and how a
benchmark's baseline configuration runs them.

Often the program knows the number before any of those threads exists:
the loop is `for (i = 0; i < n; ++i) spawn(...)`, `n` was read earlier,
and nothing changes it. The question was how to use that.

## Decision

Where every thread on a side is started by one such counted loop -- a
unit step, the spawn in every iteration unconditionally, nothing else
repeating around it, in a function that runs once -- the analysis records
the loop on the queue, and the specialisation pass turns the
constructor's tag into a choice: the loop's bound, read where the queue
is built, selects the implementation the static verdict would give at
each count. The rules that make the bound read there the one the loop
runs with are checked, not assumed: the constructor is in the loop's
function, before the loop and not in a loop around it, and every store
to the bound precedes the constructor in that function, with the bound's
address going nowhere else. A queue that fails any of them keeps its
static verdict, and a remark says which rule failed.

Nothing else changes. The shim already selects its implementation from
the tag when the queue is built, so the choice costs one load, one
comparison and a select per queue, once. Every operation on such a queue
records the general-purpose implementation, which is what every later
check may assume of any queue.

## Amended 30 Oct 2026: one call away

The lifted programs put the loop or the constructor one call from the
other: ferret's stages start in its thread pool's loop over a count main
passes, and spdlog's pool builds the queue object whose constructor
builds the queue. Either may now be one call away. A loop one call down
is read through the call: its bound must be a parameter the function
keeps as it is, and the call must pass a variable for it, which is then
the bound, held to the rules above at the call in place of the loop; a
side counts only when all its threads come through the same call. A
constructor one call down is reached through the one call of its
function that the loop's function makes, and only when that
constructor runs once in the whole program, in that call: a loop's
function that runs twice builds the queue twice, the second time under
the threads the first run's loop started, and the second run's count is
not theirs. The same holds for a constructor in the loop's own function
and for the object whose member it builds. That function is copied with
the tag as one more parameter, and the call is redirected to the copy,
so that other callers keep the static tag. A loop that may leave early
(a return, break or continue around the spawn) is counted as well: it
starts at most one thread per iteration, so at most one when it runs at
most once, which is all the choice reads.

## What it costs

- **Only one shape of count.** A loop with a stride, or a bound computed
  in the condition, is not counted, and neither is a pool whose size
  reaches the loop through a member written in another function, or
  through more than one call. Each would need its own proof that the count read at the
  constructor is the count the loop runs.
- **The queue must be built after the count is known.** A queue built by
  a global's constructor, before `main` reads the count, cannot choose.
  For ferret the port builds its queues in `main` where PARSEC initialises
  its own; a program that builds them earlier has to be changed to
  benefit, and the remark says so.
- **One of two implementations at run time.** The choice is between the
  implementations the verdict gives at one thread and at more. Nothing is
  chosen per element of an array of queues, or per call of a function
  that builds a queue.

## Options not taken

- **Retag after construction.** Build the queue generic and switch its
  implementation at the first spawn, once the count is known. It would
  reach queues built by global constructors, but needs a proof that no
  thread has touched the queue before the switch, and a shim operation
  that exists only for the compiler. Building the queue where the
  program initialises it is the simpler fix where it applies.
- **Clone the threads' functions per implementation.** Specialise the
  stage functions twice and pick the clone at the spawn. The code grows
  with every queue a function touches, and since the shim dispatches on
  the tag in every operation anyway, a second copy would buy only that
  branch.
- **Leave it to the programmer.** A topology the program declares
  is checked against the threads, and a program can pass
  `n == 1 ? topology::spsc : topology::generic` itself. But a tag that
  is not a constant is not checked, so the program would carry the risk
  the analysis exists to take away.
