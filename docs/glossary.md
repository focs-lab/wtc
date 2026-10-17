# Glossary

Every abbreviation and named thing this repository uses.

## Compiler infrastructure

| Term | Meaning |
| --- | --- |
| LLVM | A collection of compiler libraries and tools. They all live in one repository, `llvm-project` |
| Clang | The C and C++ front end inside `llvm-project` |
| LLVM IR | The low-level intermediate representation LLVM's optimisations run on |
| MLIR | Multi-Level Intermediate Representation. The framework inside `llvm-project` for building your own intermediate representations |
| Dialect | In MLIR, a set of operations, types and attributes sharing a prefix. `wtc.queue_push` belongs to the `wtc` dialect |
| ClangIR, or CIR | Clang Intermediate Representation. An MLIR-based representation Clang builds from the AST before lowering to LLVM IR. It still has the shape of the source in it |
| AST | Abstract Syntax Tree. The parse tree the front end builds |
| CIRGen | The part of Clang that produces CIR from the AST |
| LLVM incubator | A repository in the `llvm` GitHub organisation for a subproject not yet in `llvm-project`. ClangIR was one, at `github.com/llvm/clangir`. WTC does not use it: the parts WTC needs are upstream |
| Upstream | The main `llvm-project`, where work eventually lands |
| In-tree, out-of-tree | Inside the `llvm-project` tree, or a separate project alongside it. WTC is out-of-tree |
| TableGen, ODS | The language dialect operations and types are declared in. C++ is generated from it |
| Pass | One unit of analysis or transformation over the IR |
| lit, FileCheck | LLVM's test runner and its output-matching tool. A test is a file carrying the command to run and the lines expected |
| XFAIL | A marker in a lit test meaning "expected to fail". It does not turn the run red |
| ccache | A compilation cache. It makes rebuilds dramatically cheaper |

## Projects referred to as precedent

| Name | What it is |
| --- | --- |
| CIRCT | Circuit IR Compilers and Tools. An LLVM incubator applying MLIR to hardware design. Cited here as a layout precedent: it keeps `llvm-project` as a submodule and carries no patches of its own |
| IREE | An MLIR-based compiler and runtime for machine learning models, from Google. Cited as the opposite case: it does carry its own patches, and maintains a dedicated integration process to afford them |
| oneTBB, Folly | Concurrency libraries from Intel and Meta. The baselines to measure against |

## Git and GitHub

| Term | Meaning |
| --- | --- |
| SHA | A commit identifier |
| Submodule | A reference from this repository to one exact commit of another |
| Pin | Fixing a submodule at one exact commit |
| CODEOWNERS | A file assigning mandatory reviewers by path |

## Concurrency

| Term | Meaning |
| --- | --- |
| SPSC, MPSC, SPMC, MPMC | Single or Multi Producer, Single or Multi Consumer. How many threads push to a queue and how many pop |
| RCU | Read-Copy-Update. Readers proceed without synchronisation; a writer publishes a new copy and the old one is freed once no reader can be looking at it |
| SMR | Safe Memory Reclamation. The general problem of freeing memory another thread may still be reading |
| EBR | Epoch-Based Reclamation. One SMR scheme, based on advancing a global epoch |
| Hazard pointers | Another SMR scheme, where a thread publishes what it is about to read |
| Flat combining | Under heavy contention, threads publish requests and one thread executes the batch instead of each taking the lock in turn |
| Cohort lock | A hierarchical lock that keeps contention inside a socket before going global |
| Linearisation point | The instant at which a concurrent operation takes effect, as far as any observer can tell |
| TSan | ThreadSanitizer. A dynamic data-race detector |
| NUMA | Non-Uniform Memory Access. An architecture where reaching another node's memory costs more than reaching your own |
| p99, p99.9 | Latency percentiles: the value below which 99 and 99.9 percent of operations fall. Tail latency, as opposed to the mean |

## This project

| Term | Meaning |
| --- | --- |
| WTC | Well-tempered Compiler for Multithreading. The project, the dialect mnemonic, the C++ namespace and the `wtc-opt` tool |
| Shim | The thin header-only library whose calls the compiler recognises and lifts into the dialect |
| Lifting, lowering | Turning calls into dialect operations, and turning them back into calls |
| ADR | A record of an architectural decision. Here, the `decisions/` directory |

## Memory layout by owner

| Term | Meaning |
| --- | --- |
| Cache line | The unit the hardware moves between cores and caches: 64 bytes on the hosts here. Two cores writing one line take turns owning it |
| False sharing | Two threads touching different data that happens to lie in one cache line, so the line travels between their cores although neither needs the other's data. Distinguished from *true* sharing, where both touch the same data |
| Thread instance | A function under one chain of call sites from a root (main, a global constructor, a spawn), with the thread it runs on and how many times it runs. The unit the ownership walk reasons about |
| Multiplicity, `One` / `Many` | How many dynamic threads, or how many runs of a function, one instance stands for. A spawn in a loop stands for many threads |
| Start-up phase | The part of main and the global constructors that runs before the first call that can start a thread. Writes there cannot contend with threads that do not exist yet |
| Speculative thread | A thread the module cannot account for: a function whose address is taken, or an externally visible function nobody in this translation unit calls (an *orphan*). It may contribute `possible`, never `definite` |
| `definite` | Two threads the module accounts for and can tell apart repeatedly touch two fields of one object, at least one of them writing, outside the start-up phase, and the fields share a cache line in at least one of the starting positions the object's alignment allows. Across elements: a field the owner of one element hot-writes and a field the owner of the next hot-touches share a line |
| `possible` | A pair the analysis cannot rule out: the threads may be one standing for many, may be unaccounted for, may not both exist, or may have an access rate it cannot tell; or the pair would be cold but for accesses the once-side's thread has that could not be placed; or the object is a stack slot whose address escapes, or an allocation that may run more than once; or the two accesses are at elements of an array the analysis cannot tell apart |
| `cold` | A resolved claim that a pair costs nothing: different threads, but one side touches the line once as far as the analysis can see. Not printed when that side's thread has an access the analysis could not place or at an element it cannot name, or when any thread has a recurrence the walk did not expand |
| Place | Where an access lands: a root -- a global, a stack slot, or the allocator call that made a heap object -- and what its offset is relative to: the root's start, the start of the element the spawn loop handed the thread (*one element per thread*), or the start of some element the analysis cannot name (*element not known*) |
| Allocation site | The `malloc`, `calloc`, `aligned_alloc`, `memalign` or `operator new` call that made a heap object, behind any wrapper whose every return hands it back; the object's name in a remark, `calloc() in @init`. Whether it runs once is read from the call graph |
| One element per thread | A place reached as `&arr[i]` at a spawn inside a loop whose induction variable is `i`: every thread the spawn starts gets its own element, and its offsets are relative to it. A thread *owns* its element alone when the loop steps by one, spawns on every iteration, is the only repetition around the spawn and sits in a function that runs once; only then are neighbouring elements' threads both known to exist |
| Element not known | A place reached through an element step whose index is neither a constant nor the spawn loop's variable: the access is on the array, at an offset within some element. The same field pairs with itself there, since on two neighbouring elements it sits on one line, and every pair is at most possible. Counted apart in the summary |
| Unidentified access | A member access whose object the analysis could not resolve to a global, a stack slot or an allocation site: through a pointer loaded from a field, from a global written twice, from a `realloc`, or cast from an integer. Counted in every summary, because that count is the measure of the analysis's reach |
| One of several objects | A pointer loaded from a stack slot, a pointer global or a field written in several places, each of whose stored values the walk can place: the access is recorded on every object the stores name, as a member of a set. A pair through it is `possible`, never `definite`, and a repeated access through it keeps a pair on that object from being `cold`. The set says nothing when one source cannot be placed, when the object is written some other way (assigned or copied whole, initialised, seen through a view of another type, handed to code the module cannot see, a union), or past 8 sources |
