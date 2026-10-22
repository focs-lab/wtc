# Related work

What the compiler does, claim by claim, against where each technique was
done before and what is different here. Every entry below was checked
against the paper's own page, its publisher's record or the project's
repository on 23 Oct 2026; the list at the end carries the links. Papers
collected earlier are under `related-works/` and are not listed here
unless one of the sections needs them.

Four of the project's claims have no prior work that makes them: choosing a
queue's implementation per object from a topology the compiler proved,
choosing it when the object is built from the bound of a counted spawn
loop, storing the message in the channel's cell on a proven ownership, and
inferring a non-atomic reference count for a C++ `shared_ptr` statically.
Three of its techniques are not new and are presented as what they are:
lock removal by confinement, lock coarsening, and padding against false
sharing. The sections follow that order.

## 1. Choosing a data structure's implementation per object

Where it was done: for sequential collections, by profile, in Java; for
concurrent channels, by a flag the programmer sets. The compiler proving
the topology of a particular object and choosing for it has not been done.

| Technique | Where | What WTC does differently |
|---|---|---|
| Collection implementation chosen from a run-time profile of each allocation site | Chameleon (PLDI 2009); Brainy (PLDI 2011); CollectionSwitch (CGO 2018) | the choice is proved, not profiled: the set of threads that can reach each queue, from the program's spawns, so a wrong choice cannot happen on another input; and the structures are concurrent, where the choice changes the synchronisation, not the memory layout |
| Concurrent data structure synthesised from relational specifications, with lock placement chosen by an autotuner | Concurrent Data Representation Synthesis (PLDI 2012) | starts from an existing program and a fixed API, with no specification to write; the fact decided is how many threads are on each side, not how the relations are laid out |
| Single-consumer channel selected by a flag: `SingleReader` picks `SingleConsumerUnboundedChannel` | .NET `System.Threading.Channels` (`Channel.CreateUnbounded`) | the flag is inferred and checked; a program that sets the wrong one in .NET gets no diagnosis |
| Mailbox type declared in configuration: `SingleConsumerOnlyUnboundedMailbox` | Akka `akka.dispatch` (API 2.3 to 2.4; Pekko carries the same class) | the same: declared by a person, unchecked |
| One channel algorithm for every topology, on an infinite array with fetch-and-add counters | Fast and Scalable Channels in Kotlin Coroutines (PPoPP 2023) | the general-purpose queue here is of that family (a segmented fetch-and-add queue); WTC adds the step the library cannot take, replacing it with a single-producer ring where the program allows |
| Proposal to make filling a buffered channel from a slice one bulk copy, with batched sends offered as the alternative; a comment adds that a send on a channel that has not escaped could be inlined without its lock | Go issue 51553 (`cmd/compile,runtime: optimize channel filling idiom`) | the observation in the comment, that escape decides the synchronisation, carried through for every queue and both sides, rather than for one idiom |
| The standard channel's implementation replaced wholesale by a multi-producer multi-consumer one | Rust `std::sync::mpsc` on `crossbeam-channel` (rust-lang/rust PR 93563, 2022) | Rust's `Receiver` is single-consumer by type, so the library knows one side; WTC proves both sides and the ownership of the element, which no type states |

## 2. Removing or weakening synchronisation by a proof of confinement

Where it was done: thoroughly, for Java, in the late 1990s; by speculation,
in the JVM, since deprecated; for reference counts, dynamically. None of it
applies to C++ as compiled today, and the parts of WTC that do this on
`std::mutex` and `std::shared_ptr` are a follow-up, not the paper's centre.

| Technique | Where | What WTC does differently |
|---|---|---|
| Escape analysis finds objects reachable from one thread; their monitors are removed | Escape Analysis for Java (OOPSLA 1999): 11% to 92% of lock operations removed; Removing Unnecessary Synchronization in Java (OOPSLA 1999) | the same reasoning on C++ pthread and `std::mutex` objects is the follow-up thesis; what is done here already is the stronger version of the same fact for a queue, how many threads on each side, which no Java analysis computed because no Java collection changes implementation by it |
| Biased locking: a monitor's first owner takes it without an atomic operation, speculatively | HotSpot; deprecated and disabled by JEP 374 (JDK 15, 2020): its gains "far less evident", its cost in maintenance too high | an argument for a static proof over a speculation: WTC decides at compile time and never has to revoke |
| Lock coarsening: several acquisitions of one lock merged into one | Lock Coarsening (JPDC 1998; LCPC 1996), which already warns that over-coarsening serialises and measures its cost | WTC's batching of critical sections is the same transformation; its contribution can only be the profitability model, measured in W8 (losing from about 200 ns of work between sections), for which the 1998 paper gives no threshold |
| Reference count updated non-atomically by the owning thread, atomically by others | Biased Reference Counting (PACT 2018); adopted by CPython's free-threaded build (PEP 703) | dynamic, with a per-object owner; WTC would prove the owner statically for `shared_ptr` and emit no check at all; the libstdc++ dispatch (`__libc_single_threaded`, PR 96817, 2020; `__gthread_active_p` before it) is per process, not per object |
| Non-atomic count where the object cannot be shared, by analysis | Dynamic Atomicity: Optimizing Swift Memory Management (DLS 2017): up to 220% on microbenchmarks; Counting Immutable Beans (IFL 2019) and Perceus (PLDI 2021), borrowed references and reuse in functional languages | the Swift work is a run-time check; the functional work has a type system to lean on; C++ has neither, and the question is whether its pointers leave enough precision, which the dynamic census here measures first |
| The committee's view that compilers may optimise atomics under the as-if rule | N4455 (2015) and P0062 | WTC's read-modify-write to plain store on confined data is such an optimisation, with the proof the papers ask for; it is not measured yet |

## 3. Message memory crossing threads

Where it was done: in allocators built for producer-consumer traffic, in
languages with ownership types, and by hand in low-latency systems. That the
compiler decides where a message lives from a proof about its consumer is
the open part, and it is where the measured gain is: in an untimed probe
of 22 Oct an element in the cell beat the best allocator by a large factor,
which the `eval-ablation` leg is to measure.

| Technique | Where | What WTC does differently |
|---|---|---|
| Allocator in which a free on another thread is a message back to the allocating thread's slab, batched and lock-free | snmalloc: A Message Passing Allocator (ISMM 2019) | the best an allocator can do when the element must cross; WTC avoids the crossing: the cell is the queue's, allocated once, and only the queue's memory moves. snmalloc is a baseline in the `eval-w1` and `eval-ablation` legs; the others run two or three allocators |
| Static analysis decides which data may be sent as a message and allocates it in a shared area | Message Analysis for Concurrent Programs Using Message Passing (TOPLAS 2006), for Erlang | the same question for C++ queues, answered the other way round: not where to allocate the message, but whether the consumer keeps it, so that it need not be allocated at all |
| Isolation types: a message has one owner at a time, checked statically | Kilim (ECOOP 2008); Singularity's channels and exchange heap (EuroSys 2006) | types the programmer writes; WTC infers the ownership (borrow, transfer, share) from the consumer's code and chooses the storage |
| Data structures split into pools by a context-sensitive pointer analysis | Automatic Pool Allocation (PLDI 2005) | per data structure, not per thread pair; the pool here is the queue's own ring, and the decision is whether the element can live in it |
| Pre-allocated entries in a ring, reused for every message | LMAX Disruptor (repository and documentation) | the technique, by hand, in Java; WTC applies it where it proves it safe, with the consumer's borrow checked |
| Single-producer ring with cache-aware temporal slipping; batched ring with backtracking | FastForward (PPoPP 2008); B-Queue (IJPP 2013) | the ceiling WTC's ring is measured against and, since 23 Oct, built on: its consumer keeps a batch behind the producer and polls at the producer's rate |
| Fetch-and-add ring queues as the general-purpose ceiling | LCRQ, Fast Concurrent Queues for x86 Processors (PPoPP 2013); LPRQ, The State-of-the-Art LCRQ Concurrent Queue Algorithm Does NOT Require CAS2 (PPoPP 2023), with its artifact (LCRQ, LPRQ, LSCQ, FAAArrayQueue, CC-Queue) | the general-purpose queue here is a segmented fetch-and-add queue of this family; the artifact's queues are baselines in W9 |

## 4. Stream and pipeline compilers

Where it was done: with a language for streams, or a compiler extracting
the pipeline; the questions were partition, placement and the number of
threads per stage. WTC takes the program's threads and queues as written
and asks a narrower question about each queue. Placement and stage fusion
were measured here and set aside: the placement effect disappears once the
element is in the cell, and fusion only pays for stages under about 30 ns.

| Technique | Where | What WTC does differently |
|---|---|---|
| Stream language compiled to cores with task, data and pipeline parallelism | StreamIt (ASPLOS 2006): 11.2x on 16 cores | a language; WTC reads pthreads and `std::thread` |
| Pipeline extracted from sequential loops by the compiler | Decoupled Software Pipelining (MICRO 2005) | the pipeline exists already; the queue's implementation is what changes |
| Cores assigned to stages by run-time feedback | Feedback-Directed Pipeline Parallelism (PACT 2010), on ferret and dedup among others; DoPE (PLDI 2011); Parcae (PLDI 2012) | the number of threads per stage is the program's; WTC proves what that number is on each side of each queue, and when it is set at run time, chooses the implementation then |

## 5. False sharing

Not new. The compiler's alignment of fields by their owning thread is the
transformation the detectors below repair with; the only thing WTC adds is
that the thread walk already has the owners. On memcached no pair was found.

| Technique | Where |
|---|---|
| Detection by running threads as processes and comparing their writes; repair by isolating updates | Sheriff (OOPSLA 2011) |
| Prediction from one execution, for other layouts and line sizes | Predator (PPoPP 2014) |
| Hybrid detection and repair, grouping data by the threads that access it: 3.82x on average | Huron (PLDI 2019) |
| Detection in the coherence protocol | Leveraging Cache Coherence to Detect and Repair False Sharing On-the-fly (MICRO 2024) |

## 6. Progress and nonblocking checks

| Technique | Where | What WTC does differently |
|---|---|---|
| `[[clang::nonblocking]]` and `[[clang::nonallocating]]` verified at compile time; RealtimeSanitizer at run time | Clang Function Effect Analysis (Clang 20); RealtimeSanitizer | the same check, but its verdict on a queue operation depends on the implementation the compiler chose: a pop of the general-purpose queue blocks, a pop of the ring does not, a pop that returns a `shared_ptr` allocates, a transfer does not; the check is a consequence of the substitution, not a contribution of its own |
| Static detection of sleeping in atomic context in kernel modules: 401 bugs | DSAC (USENIX ATC 2018) | the same shape of reachability check, on a different property |
| Thread roles declared and checked for consistency with the code | Composable Thread Coloring (PPoPP 2010) | the roles are inferred here; the declared-topology check (a tag written by hand against the proved threads) is the nearest thing to a coloring check, and is what thesis C would extend |

## 7. Raising to a dialect

| Technique | Where | What WTC does differently |
|---|---|---|
| Fork-join parallelism represented in the compiler's IR so that it can be optimised across | Tapir (PPoPP 2017) | the same argument for queues, mutexes and thread spawns: as calls they are opaque, as operations they carry a contract (`decisions/0003`) and a progress answer |
| C raised to MLIR for polyhedral transformation; CUDA raised and retargeted to CPU threads, beating handwritten OpenMP by 58% on Rodinia | Polygeist (PACT 2021); High-Performance GPU-to-CPU Transpilation and Optimization via High-Level Parallel Constructs (PPoPP 2023) | WTC raises through ClangIR and lifts calls into a shim into dialect operations; its subject is shared-memory communication, which neither covers |
| SYCL modelled in MLIR dialects to analyse host and device together: up to 4.3x | Experiences Building an MLIR-Based SYCL Compiler (CGO 2024) | a model of the paper's shape: a dialect that lets the compiler see what the library hid |
| Static analyses in LLVM remove redundant ThreadSanitizer instrumentation on Memcached, Redis, SQLite and MySQL | Compiling Away the Overhead of Race Detection (arXiv 2512.05555, 2025) | the same facts about threads and synchronisation serve a different end; a WTC analysis that proves confinement could feed that instrumentation (idea 18 of the audit) |

## What is open

No prior work found, after the sweep above and the audit of 22 Oct:

- an implementation chosen per queue object from the set of threads the
  compiler proved on each side, with the general one kept where it cannot;
- that choice made when the object is built, from the bound of a counted
  spawn loop (`decisions/0005`): the shape of spdlog's thread pool, whose
  number of workers is only known when the pool is built;
- the message stored in the channel's cell, lent, transferred or shared,
  by a proof about the consumer's code rather than a type or a flag;
- a `std::shared_ptr` whose count is proved to be touched by one thread and
  made non-atomic statically, in C++.

## Threats

- Rust's `Receiver` and .NET's `SingleReader` show that the topology can be
  carried by a type or a flag; the answer is that both are declared and
  neither is checked, and that neither decides where the element lives.
- Kotlin's channels and the LPRQ family show that a general-purpose queue
  can be fast; the ablation must keep showing that the cell, not the
  synchronisation, is most of the gain, under snmalloc as well as glibc.
- The confinement proofs for `std::mutex` and `shared_ptr` are escape
  analysis of 1999 on a new language; they are presented as a follow-up,
  with the dynamic census first, and not as the paper's claim.
- Lock coarsening and false-sharing padding are old; they are kept as
  consequences of the thread walk, and the README says so.

## Sources

Checked on 23 Oct 2026 against the pages linked.

- Chameleon: Adaptive Selection of Collections. PLDI 2009. <https://doi.org/10.1145/1542476.1542522>
- Brainy: Effective Selection of Data Structures. PLDI 2011. <https://doi.org/10.1145/1993498.1993509>
- CollectionSwitch: A Framework for Efficient and Dynamic Collection Selection. CGO 2018. <https://doi.org/10.1145/3168825>
- Concurrent Data Representation Synthesis. PLDI 2012. <https://doi.org/10.1145/2254064.2254114>
- .NET `Channel.CreateUnbounded` and `UnboundedChannelOptions.SingleReader`. <https://source.dot.net/System.Threading.Channels/System/Threading/Channels/Channel.cs.html>
- Akka `SingleConsumerOnlyUnboundedMailbox`. <https://doc.akka.io/japi/akka/2.4/akka/dispatch/SingleConsumerOnlyUnboundedMailbox.html>
- Fast and Scalable Channels in Kotlin Coroutines. PPoPP 2023. <https://doi.org/10.1145/3572848.3577481>
- cmd/compile,runtime: optimize channel filling idiom. golang/go issue 51553. <https://github.com/golang/go/issues/51553>
- Merge crossbeam-channel into `std::sync::mpsc`. rust-lang/rust PR 93563. <https://github.com/rust-lang/rust/pull/93563>
- Escape Analysis for Java. OOPSLA 1999. <https://doi.org/10.1145/320385.320386>
- Removing Unnecessary Synchronization in Java. OOPSLA 1999. <https://doi.org/10.1145/320384.320388>
- JEP 374: Deprecate and Disable Biased Locking. <https://openjdk.org/jeps/374>
- Lock Coarsening: Eliminating Lock Overhead in Automatically Parallelized Object-Based Programs. JPDC 49, 1998. <https://doi.org/10.1006/jpdc.1998.1441>
- Biased Reference Counting: Minimizing Atomic Operations in Garbage Collection. PACT 2018. <https://doi.org/10.1145/3243176.3243195>
- PEP 703: Making the Global Interpreter Lock Optional in CPython. <https://peps.python.org/pep-0703/>
- libstdc++: Use `__libc_single_threaded` to optimise atomics (PR 96817). <https://gcc.gnu.org/pipermail/gcc-patches/2020-October/555234.html>
- Dynamic Atomicity: Optimizing Swift Memory Management. DLS 2017. <https://sigplan.org/OpenTOC/dls17.html>
- Counting Immutable Beans: Reference Counting Optimized for Purely Functional Programming. IFL 2019. <https://arxiv.org/abs/1908.05647>
- Perceus: Garbage Free Reference Counting with Reuse. PLDI 2021. <https://pldi21.sigplan.org/details/pldi-2021-papers/7/Perceus-Garbage-Free-Reference-Counting-with-Reuse>
- N4455: No Sane Compiler Would Optimize Atomics. WG21, 2015. <https://wg21.link/n4455>
- snmalloc: A Message Passing Allocator. ISMM 2019. <https://conf.researchr.org/details/ismm-2019/ismm-2019-papers/3/snmalloc-A-Message-Passing-Allocator>
- Message Analysis for Concurrent Programs Using Message Passing. TOPLAS 28(4), 2006. <https://doi.org/10.1145/1146813>
- Kilim: Isolation-Typed Actors for Java. ECOOP 2008. <https://www.cl.cam.ac.uk/~am21/papers/ecoop08.pdf>
- Language Support for Fast and Reliable Message-based Communication in Singularity OS. EuroSys 2006. <https://www.microsoft.com/en-us/research/?p=150251>
- Automatic Pool Allocation: Improving Performance by Controlling Data Structure Layout in the Heap. PLDI 2005. <https://doi.org/10.1145/1064978.1065027>
- LMAX Disruptor. <https://github.com/LMAX-Exchange/disruptor>
- FastForward for Efficient Pipeline Parallelism: A Cache-Optimized Concurrent Lock-Free Queue. PPoPP 2008. <https://scholar.colorado.edu/downloads/v405sb189>
- B-Queue: Efficient and Practical Queuing for Fast Core-to-Core Communication. IJPP 41, 2013. <https://doi.org/10.1007/s10766-012-0213-x>
- Fast Concurrent Queues for x86 Processors. PPoPP 2013. <https://doi.org/10.1145/2442516.2442527>
- The State-of-the-Art LCRQ Concurrent Queue Algorithm Does NOT Require CAS2. PPoPP 2023. <https://ppopp23.sigplan.org/details/PPoPP-2023-papers/2/The-State-of-the-Art-LCRQ-Concurrent-Queue-Algorithm-Does-NOT-Require-CAS2>; artifact <https://zenodo.org/records/7337237>
- Exploiting Coarse-Grained Task, Data, and Pipeline Parallelism in Stream Programs. ASPLOS 2006. <https://www.microsoft.com/en-us/research/?p=341351>
- Automatic Thread Extraction with Decoupled Software Pipelining. MICRO 2005. <https://collaborate.princeton.edu/en/publications/automatic-thread-extraction-with-decoupled-software-pipelining/>
- Feedback-Directed Pipeline Parallelism. PACT 2010. <https://utw10235.utweb.utexas.edu/pub/suleman_pact10.pdf>
- Parallelism Orchestration Using DoPE: The Degree of Parallelism Executive. PLDI 2011. <https://doi.org/10.1145/1993316.1993502>
- Parcae: A System for Flexible Parallel Execution. PLDI 2012. <https://liberty.princeton.edu/Publications/pldi12_parcae.pdf>
- Sheriff: Precise Detection and Automatic Mitigation of False Sharing. OOPSLA 2011. <https://2011.splashcon.org/details/oopsla-2011-papers/1/Sheriff-Precise-Detection-and-Automatic-Mitigation-of-False-Sharing>
- Predator: Predictive False Sharing Detection. PPoPP 2014. <https://people.cs.umass.edu/~emery/pubs/Predator-ppopp14.pdf>
- Huron: Hybrid False Sharing Detection and Repair. PLDI 2019. <https://doi.org/10.1145/3314221.3314644>
- Leveraging Cache Coherence to Detect and Repair False Sharing On-the-fly. MICRO 2024. <https://doi.org/10.1109/MICRO61859.2024.00066>
- Clang Function Effect Analysis. <https://clang.llvm.org/docs/FunctionEffectAnalysis.html>; RealtimeSanitizer. <https://clang.llvm.org/docs/RealtimeSanitizer.html>
- DSAC: Effective Static Analysis of Sleep-in-Atomic-Context Bugs in Kernel Modules. USENIX ATC 2018. <https://www.usenix.org/conference/atc18/presentation/bai>
- Composable Thread Coloring. PPoPP 2010. <https://doi.org/10.1145/1693453.1693485>
- Tapir: Embedding Fork-Join Parallelism into LLVM's Intermediate Representation. PPoPP 2017. <https://www.csail.mit.edu/node/7782>
- Polygeist: Raising C to Polyhedral MLIR. PACT 2021. <https://research.google/pubs/polygeist-raising-c-to-polyhedral-mlir/>
- High-Performance GPU-to-CPU Transpilation and Optimization via High-Level Parallel Constructs. PPoPP 2023. <https://ppopp23.sigplan.org/details/PPoPP-2023-papers/10/High-Performance-GPU-to-CPU-Transpilation-and-Optimization-via-High-Level-Parallel-Co>
- Experiences Building an MLIR-Based SYCL Compiler. CGO 2024. <https://arxiv.org/abs/2312.13170>
- Compiling Away the Overhead of Race Detection. arXiv 2512.05555, 2025. <https://arxiv.org/abs/2512.05555>
