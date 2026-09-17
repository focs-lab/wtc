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
