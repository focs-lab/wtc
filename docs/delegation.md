# Delegating a hot mutex's critical sections

A mutex that every operation of a program takes, on many cores, moves the
lines it guards from core to core at every hand-over, and puts the threads
that lose the race to sleep in the kernel. Running its critical sections on
one server thread instead keeps those lines in one cache: a client hands
over a request and waits for it, and only the request and its completion
cross between cores. The probe of 23 Nov 2026 did this by hand for
LevelDB's DB mutex (`DBImpl::Get`, readrandom), and memcached's set path,
split over four small locks, lost outright. The timed leg of 24 Nov 2026
(`benchmarks/results/2026-11-24-d4-delegate/`, 24 processors at a fixed
1.90 GHz, median of 10) measured the compiler's build: from eight threads
on it runs 1.43-1.79x the original and 1.10-1.15x the hand-delegated
patch, but only 0.96-1.10x the best LiTL lock (MCS), which the untimed
probe had at 1.45x. Below eight threads it loses: 0.51x the original at
one thread, 0.82x at four. The server costs about one more core.

`--wtc-delegate-sections` (`mlir/lib/WTC/Transforms/DelegateSections.cpp`)
does it in the compiler: for one mutex the build names, it finds the
critical sections, proves that each cannot tell which thread runs it, or
refuses it with the reason, and hands those it proves to a server thread
for the mutex (`runtime/delegate.hpp`). Sections it refuses stay as
written, under the same mutex, which the server takes as well.

## Opt-in, and when to ask for it

Nothing is delegated unless the build names the mutex:

```console
$ tools/wtc-cc --delegate leveldb::DBImpl::mutex_ --delegate-assume-uniform-threads \
    -O2 -fno-exceptions ... -o db_bench
$ wtc-opt m.cir --wtc-delegate-sections="mutex=leveldb::DBImpl::mutex_"
```

A mutex is named as CIR names its member, `Class::member`, or as a global,
`name`; `--delegate` may be given more than once. The pass has no cost
model: whether delegating pays is the build's decision, and the leg says
when it does. One mutex that every operation takes, with sections of a
few hundred cycles on lines they share, and more contending cores than
sections per operation: LevelDB's DB mutex from eight threads on. Not at
four threads or fewer, where the hand-over is overhead with little to
save (a client always hands over, even when the mutex is free), and not
where an operation makes several small sections (memcached 1.6's set
path, about seven round trips per set, which saturated the server). The
server takes a core while it has work; when it has none it spins briefly
and then sleeps, costing nothing until a client wakes it.

## What the pass does

A section is the code from a lock of the mutex to its unlock on every path
from the lock. The lock and the unlock may be written as:

- `std::lock_guard`, `std::unique_lock` (an early `unlock()` ends the
  section), `std::scoped_lock`, or `lock()` and `unlock()` on the mutex;
- `pthread_mutex_lock` and `pthread_mutex_unlock` on it;
- a function of the program's whose only lock operation, through what it
  calls, is one lock or one unlock of what a parameter points to
  (LevelDB's `port::Mutex::Lock`), and a lock object of the program's
  whose constructor keeps the mutex it is handed and locks it and whose
  destructor unlocks the one it kept (LevelDB's `MutexLock`).

Wrappers are read from their bodies, never their names: each function gets
a summary of the lock operations its callers see, which parameter, or
which member of a parameter, each acts on, and the parameters it keeps in
members. The mutex is the member the option names, reached through member
steps and base classes from an object, or the global.

Real sections begin and end at different depths of a function's scopes.
`DBImpl::Get` takes the mutex with a `MutexLock` at the top, lets it go
with `mutex_.Unlock()` inside a nested block, takes it again with
`mutex_.Lock()` at the end of that block, and leaves it at the return,
whose cleanup runs the lock object's destructor. So the function is first
made a flat graph of blocks (CIR's own `cir-flatten-cfg`, with its
variables moved to its entry by `cir-hoist-allocas`), and a section is the
set of blocks reachable from the lock without passing an unlock of the same
mutex. The pass splits the blocks at the lock and after each unlock, and,
if the section passes, moves its blocks into a function of its own:

```mlir
%e = wtc.delegate %mutex, @_ZN7leveldb6DBImpl3Get...wtc.section.1(%env) {mutex_name = "leveldb::DBImpl::mutex_"}
       : !cir.ptr<!rec_leveldb3A3Aport3A3AMutex>, !cir.ptr<!void> -> !s32i
```

The section's function takes an environment, an array of the addresses of
the values the section reads (a pointer travels there itself, any other
value through a variable of the caller's) and of slots for values the code
after it reads, and returns the exit it left by; the caller branches on it.
The section's code is the program's own, the mutex's lock at its start and
its unlock at each exit included: calling the function on the calling
thread is the program as written. `wtc.delegate` means exactly that call,
and `--lower-wtc-to-cir` makes it a call of the runtime,
`wtc_delegate_run(mutex, section, env)`, which runs it on the mutex's
server. `wtc-cc` runs the pass after every analysis that reads the
structured form, since the functions it outlines from are flat afterwards,
and links `shim/delegate.cpp`, the runtime's entry point, when a section
was delegated.

## The runtime

`runtime/delegate.hpp` keeps one server thread per mutex, keyed by the
mutex's address and started by the first section delegated on it. A client
pushes a request (the section, its environment, a completion flag on its
own stack) on the server's queue, the single-consumer form of
`runtime/segment_queue.hpp`, and waits for the flag through its own
`runtime/waiter.hpp`: it spins, then parks. The server pops requests in
order and runs each. A server that reaches a delegated section, of its own
mutex or another, runs it on its own thread.

The server does not pay the waiter's fence on the common path: having set
a flag, it wakes the client only if it reads it announced as asleep. Read
without the fence, an announcement may be missed by a client just going to
sleep, so a client's park is bounded (one millisecond) and it then looks
again: a missed wake-up costs time, never progress. The fence was the
difference between the first build of this runtime and the hand-written one
of the probe: on the server it waits for the flag's store to reach the
client spinning on it, a line transfer per section. An idle server spins as
long as a client and then sleeps with no time limit; a client wakes it with
the waiter's own, fenced, notification after its push, on the client's side.

The servers' lives, and what each is started with:

- **They end with the last client.** A thread becomes a client at its first
  delegated section and stops being one when it ends; when the last client
  has ended, every server ends too, and a later client starts them again.
  A program whose main thread ends with `pthread_exit` ends when its last
  own thread does, as written. A section reached from a thread-local
  object's destructor after its thread stopped being a client runs on that
  thread.
- **At most four at once** (`WTC_DELEGATE_MAX_SERVERS`). The sections of a
  mutex delegated while as many servers run run on their client, under the
  mutex, as written, and so do they when a server cannot be started
  (`pthread_create` failing for want of memory or threads). That bounds the
  threads a class with one mutex per object can start; running a section
  as written is always faithful, it only gains nothing.
- **Every signal blocked,** from its first instruction: an asynchronous
  signal goes to a thread of the program's, never to a server, whatever the
  mask of the client that happened to start it.
- **A stack as large as its starting client's** (at least the default, at
  most 1 GiB). A section never has less stack on the server than on that
  client, and a client whose stack is larger than its server's runs its
  sections itself.
- **The affinity and scheduling of its starting client,** unless
  `WTC_DELEGATE_CPU` pins it.
- **None in a fork's child:** the child's runtime starts empty
  (`pthread_atfork`), and its first delegated section starts a server of
  its own.

The environment:

| variable | what it does |
|---|---|
| `WTC_DELEGATE_CPU=LIST` | pin the servers, in the order they start, to these CPUs; unset, a server inherits its starting client's affinity |
| `WTC_DELEGATE_WAIT=spin` | the servers never park when idle |
| `WTC_DELEGATE_SPIN_NS=N` | spin this long before parking, clients and servers (50 microseconds by default) |
| `WTC_DELEGATE_MAX_SERVERS=N` | at most N servers at once (4 by default); the sections of further mutexes run as written |
| `WTC_DELEGATE_REPORT=1` | print each server's count of sections at exit |

The runtime's tests (`runtime/test/delegate_test.cpp`) run plain, under
ThreadSanitizer and with `WTC_CHECK`: exclusion between clients and threads
that run the section as written, results and writes coming back, each
client's order, a section that reaches another delegated section, one
server per mutex, clients and servers asleep between sections,
short-lived clients whose waiters later threads take over; servers ending
with the last client, a main that ends with `pthread_exit`, the bound on
servers, their blocked signals and their stack, a fork's child, a
section from a thread-local destructor, and a client's errno, which a
delegated section leaves as it was (the runtime's own calls, the futex
a client parks on among them, may set it; the original's lock and
unlock do not).

## The contract

The claim is that delegation is faithful: every run of the delegating
program is, in everything a program may observe, a run the original
allows. What the contract takes as given:

- **A race-free program, free of undefined behaviour,** as for lifting
  (`docs/lifting.md`, "The contract"): in particular a mutex is not taken
  again by the thread that holds it, unless it is recursive.
- **Observable behaviour** is what the program writes, its exit status and
  the values its functions compute, not which thread computes them, nor
  timing.
- **The mutex's semantics.** Sections of one mutex run one at a time, in
  an order the mutex could have granted; what a thread did before a
  section happens before it, and what the section did happens before what
  the thread does after it.

What the runtime keeps, and why:

- **Exclusion.** The server runs a section by calling its function, which
  takes the mutex as the original section did; a thread that runs a
  section of the same mutex as written takes it too.
- **Happens-before.** A client's push happens before the server's pop of
  it, and the server's store of the completion flag (release) before the
  client's load of it (acquire); between sections of different clients the
  mutex orders them as before.
- **Order.** Each client waits for its section, so its sections run in its
  program order; between clients they run in the order the queue
  linearises their pushes, which an unfair mutex could have granted.
- **No new deadlock.** The server stands for its mutex. A client waits for
  it as it waited for the mutex, and while the server runs a client's
  section that client is, for every other thread, inside its section.
  Mapped that way, a cycle of threads waiting for each other in the
  delegating program is one the original could fall into too, provided the
  server runs the sections of exactly one mutex and the section cannot tell
  that another thread runs it (condition (a), and (c) for the locks a
  thread owns). A server shared by two mutexes would order their sections,
  which the original did not: a thread holding one while it delegates a
  section of the other, behind a section of the first, is a deadlock the
  original did not have (memcached's LRU maintainer, in the probe). The
  server is therefore one per mutex, keyed by its address, even when two
  objects of a class each have one.

Known differences, outside the contract:

- **Which thread runs the section.** Proved invisible by condition (a),
  except where an assumption admits it (below).
- **Progress.** A client waits for the server; if the server is
  descheduled, every client of that mutex waits for it. A thread that held
  the mutex could be descheduled as well, but only while it held it.
- **Cost.** The server takes a core while it has work and sleeps when it
  has none; under contention the clients spin up to 50 microseconds before
  they park. A program delegates through up to four more threads than it
  starts, with stacks as large as its clients'.
- **Memory placement.** Pages a section touches first are placed on the
  server's node.
- **Thread-level attributes of what a section starts.** A thread started
  inside a delegated section (only under `assume-uniform-threads`) starts
  with every signal blocked, the server's mask, and the server's affinity
  and scheduling.

## The legality proof, and what each refusal says

Deny by default: a section is delegated only when the pass proves all of
the following of it and of everything it calls, each refusal naming the
condition it fails:

```
remark: wtc: delegated section <function> #<n> on <mutex> delegated
remark: wtc: delegated section <function> #<n> on <mutex> not delegated: (<c>) <why> (through <calls>)
```

- **(a) It cannot tell which thread runs it.** No thread-local variable;
  no `errno` (`__errno_location`), nor `malloc`, `calloc`, `realloc`, the
  nothrow `operator new` and their kind, which report a failure through
  `errno`; no thread id, signal mask, signal handler, affinity,
  scheduling, thread name, cancellation state, CPU number or resource use
  of the thread (`pthread_self`, `gettid`, `syscall`, `pthread_sigmask`,
  `sigaction`, `raise`, `sched_getcpu`, `getrusage`, ...), no thread's CPU
  clock (`clock_gettime` only with a system clock it reads as a constant),
  no stack frame's address (`__builtin_frame_address`); no `setjmp`/
  `longjmp`; no thread started, unless `assume-uniform-threads` (below);
  in the section's own code, no stack memory of run-time size (a
  variable-length array, `alloca`, a stack save or restore), which would be
  on the server's stack and gone before the code after the unlock reads
  it. The global `operator new` and `operator delete` are allowed: a
  failure throws, which (b) covers.
- **(b) It is one section.** One entry, its lock; every path from the lock
  reaches an unlock of the same mutex, of the same object, without
  returning from the function, without taking the mutex again, and without
  an exception edge; a lock of the mutex inside an earlier section of the
  function is refused too; the section does not read its function's
  variadic arguments, which the outlined function has not. No exception can
  leave it: the function is
  `nothrow` (built without exceptions, or `noexcept`), or every call the
  section makes is, the mutex's own lock and unlock aside, which are taken
  not to fail. An exception would unwind the server, not the client. In a
  program built with exceptions a `std::mutex` lock or unlock is usually a
  call with a cleanup, and its section is refused.
- **(c) Its other locks are its own.** Every other lock it takes it lets go
  within itself, and it lets go no lock it did not take within itself
  before, in the order of its code (a section that drops a lock its caller
  holds and takes it back balances in count, but the server would let go
  a lock another thread holds); a call through a pointer whose possible
  targets take or let go a lock refuses, since its caller cannot count it.
  It takes no recursive mutex: a `std::recursive_mutex` by its functions'
  names, and a pthread or C11 mutex the program's own code takes (not
  through `std::mutex`, which libstdc++ never makes recursive) only where
  the pass proves none of the program's is: the module calls no
  `pthread_mutexattr_settype`, holds no constant of a pthread mutex other
  than zeros (a recursive initialiser in a global, a member of one, or a
  constructor's default member initialiser), calls `mtx_init` only with a
  constant type without `mtx_recursive`, and is the whole program. The
  named mutex is not taken for reading: readers that share a read-write
  lock as written would run one at a time on the server, one waiting for
  what another holds (its write sections, exclusive as the server is, are
  delegated). A thread owns the locks it holds: the server would wait for
  one its client holds, where the client would have taken it again.
- **(d) It waits for no other thread but by taking such a lock.** No
  condition wait, join, sleep, yield, semaphore, input or output, no lock
  with a time limit. A section that waits on the server holds up every
  client, and may wait for one of them.
- **(e) The pass sees all of its code.** A function with no body here must
  be on the audited list; an indirect call must resolve (a function pointer
  parameter, as `Utils.h`'s `indirectCallees` resolves it, or a virtual
  call, below); inline assembly may only wait or order memory (`pause`,
  fences: not `rdtscp`, `rdpid` or `cpuid`, which read the CPU), and so may
  an intrinsic (deny by default: `pause`, the fences, `prefetch`,
  `expect`, `assume`; not `rdtscp`, `rdpid`).

The audited list, functions with no body here a delegated section may
call: the lock and unlock primitives of pthread mutexes, read-write locks,
spinlocks and C11 mutexes (balanced by (c)); `pthread_cond_signal`,
`pthread_cond_broadcast`, `std::condition_variable::notify_one` and
`notify_all`; `pthread_detach` and `std::thread::detach`; `free` and the
global `operator new` and `delete` forms but the nothrow `new`; the string
and memory functions that touch only what they are handed and set no
`errno` (`memcpy`, `memset`, `strlen`, `abs`, `fabs`, ...; not `sqrt`, which
sets `EDOM`); the system's clocks (`gettimeofday`, `time`, and
`clock_gettime` with a constant system or process clock); what ends the program or throws (`abort`, `__assert_fail`,
`std::terminate`, `__cxa_throw` and the exception helpers, libstdc++'s
`std::__throw_*`); `std::thread::_State`'s destructors, which libstdc++
defines as the default; and, under `assume-uniform-threads`,
`pthread_create` and `std::thread::_M_start_thread`. Every other function
with no body here refuses, by what `Blocking.h` reads of its name ((d) for
a wait, input or output, (a) for the threading library) or as code the
pass does not see ((e)).

A virtual call is resolved only when the module is the whole program (it
has `main`, is not compiled with `-c`, and no file the pass does not see is
linked): its targets are the functions in the call's slot of every vtable
the module defines whose class may be the call's class or derive from it,
with the call's signature. The module's records do not say which members
are bases, so a class is taken to derive from the call's class when it
holds it, as a base or as a member, at any depth (more classes than derive
from it, never fewer). A vtable the module only declares, a class of the
system's libraries, may belong to the call's class only if its record is
in the module; then the call is unresolved, except for
`std::thread::_State`, which is abstract.

The tests: `test/Transforms/DelegateSections/accepts.cpp` (every form of
lock, LevelDB's `Get` reduced, results through a local, a member and a
return value, the lowering), `refusals.cpp` (each condition, and a
thread start admitted under the assumption), `exceptions.cpp` (built with
exceptions), `pthread.c` (a C program's global mutex and its lock helpers,
and a module that may make it recursive), and the findings of the review
of 23 Nov 2026: `stack_identity.c` (stack memory of run-time size,
variadic arguments, an intrinsic that reads the CPU, a frame's address,
`sqrt`'s `errno`, a thread's CPU clock), `lock_order.c` (a read lock of the
named lock, a caller's lock let go and taken back), `recursive.c` and
`recursive_member.cpp` (a recursive mutex the module may make, in a member
of a global, through C11, in a default member initialiser, or in files the
pass does not see) and `virtual_lock.cpp` (a lock taken through a virtual
call); and, through `wtc-cc`, built as
written, delegated, and delegated under ThreadSanitizer,
`test/Integration/Delegate/leveldb_get.cpp` (a reduced model of LevelDB's
DB mutex: `Get`'s two sections, a compaction scheduled through a virtual
`Env` whose first call starts the background thread, a write that waits on
a condition variable and stays inline, the background thread's section
delegated) and `counter.c`.

## Assumptions

Delegation is soundy: faithful for every program that meets the contract
and these assumptions, and nothing is claimed for any other. Breaking one
marked sound only refuses more.

- **What the summaries read is all the locking there is.** A lock
  operation is one of the primitives above or a call of a function whose
  body reaches one; balance and order are read over a function's code, not
  path by path, so a function that locks one mutex on one path and unlocks
  it on another that does not pass the lock counts as balanced. A program
  breaks it with such code.
- **The mutex's own lock and unlock do not fail.** A `std::mutex` throws
  `std::system_error` on a failure to lock, which a mutex used as the
  original uses it does not meet.
- **A pthread mutex is recursive only by what the pass reads** (a mutex
  attribute's type, a constant other than zeros, `mtx_init`'s type), in a
  whole program: a program that builds one from bytes it computes (a
  `memcpy` of a computed value into a `pthread_mutex_t`) breaks it.
- **The vtables of the module are all the program's** when it is the whole
  program, and a class of the system's libraries derives from no class of
  the program's.
- **`assume-uniform-threads`** (`wtc-cc --delegate-assume-uniform-threads`),
  when given: the program does not depend on the signal mask, affinity or
  scheduling attributes of a thread it starts inside a delegated section.
  Such a thread starts with every signal blocked (the server's mask), and
  with the affinity and scheduling the server took from the client that
  started it, when it started it: a program whose clients change theirs
  afterwards, or that pins the servers (`WTC_DELEGATE_CPU`, a measurement
  setting), gives the thread other attributes than the original would
  have. Off by default; LevelDB needs it, since its first compaction
  starts the background thread from `Get`'s second section, and that
  thread handles no signal.
- **The server is scheduled.** A client waits for it (above).

### Known limitations

What the pass refuses rather than models:

- sections whose lock and unlock are in different functions (LevelDB's
  `RemoveObsoleteFiles`, `MakeRoomForWrite`, `WriteLevel0Table`, and the
  tail of `DoCompactionWork`, all called with the mutex held: "leaves F
  with the mutex held");
- a section's lock or unlock that is a call with an exception edge, in a
  program built with exceptions;
- the mutex reached through a pointer kept in a local variable, when the
  pass must tell a lock's mutex from an unlock's: in the flattened function
  the variable's store is no longer in the block that declares it, which
  the tracing of `Utils.h` needs ("lets go a mutex of the same member that
  the pass cannot tell is the one it took": LevelDB's `DB::Open`);
- calls of libstdc++'s out-of-line members that are not on the audited
  list: `std::string`'s `_M_append`, `_M_assign`, `_M_create` and
  `_M_replace`, the red-black tree's `_Rb_tree_decrement`, and `snprintf`
  (LevelDB's `GetProperty`, `GetApproximateSizes`,
  `OpenCompactionOutputFile`, `BackgroundCall`);
- virtual calls whose possible targets include a function pointer call
  (LevelDB's C API wraps user callbacks as `Comparator` and
  `FilterPolicy` subclasses, so a comparison through `Comparator*`
  reaches them: `RecordReadSample`, `NewInternalIterator`), and those over-
  approximated through a class that holds the call's class as a member
  (`ReleaseSnapshot`'s `delete` of a `Snapshot*` reaches `DBImpl`'s
  destructor, since `DBImpl` holds a `SnapshotList`, which holds a
  `SnapshotImpl`);
- `malloc` and its kind in a section, for `errno`, and `sqrt` and the rest
  of the math library;
- read sections of a read-write lock named for delegation.

## LevelDB

LevelDB 1.23 (the corpus clone, 7ee830d), its library and `db_bench` built
as one program through `wtc-cc` (`-O2 -fno-exceptions -fno-rtti`; every
one of its files goes through ClangIR, and the build runs): with
`--delegate leveldb::DBImpl::mutex_ --delegate-assume-uniform-threads`,
three sections are delegated, `Get`'s two and `GetSnapshot`'s, and
thirty-three are refused, each with its reason: condition waits (`Write`, `~DBImpl`,
the `TEST_` functions), sections that end in another function, and the
limitations above. Without the assumption `Get`'s second section is
refused under (a). `Write` and the compaction keep the mutex as written,
and exclude the server's sections by it. ThreadSanitizer on the delegated
build, readrandom and readwhilewriting at four threads, 320 000 delegated
sections: no report, as for the build as written.

How fast it is will be a timed leg; untimed checks so far put the
compiler's build level with the probe's hand-written delegation at eight
threads, and below the build as written at four, as the probe found for
the hand-written one.
