# Documentation

| File | What it is |
| --- | --- |
| `vision.md` | Why the project exists and the transformations it intends to perform |
| `audit.md` | Known defects in the current code: September 2026 for the queue analysis, October for the layout passes |
| `glossary.md` | The terms and abbreviations this repository uses |
| `gate.md` | The first gate: one producer, one consumer, proved on real C++ and measured |
| `mvp.md` | What the compiler does now, with a command and its remarks for each transformation |
| `evaluation.md` | The evaluation: its questions, baselines and method, the results so far, and the threats to them |
| `corpus.md` | What people pad by hand, counted across 17 codebases, and what the false-sharing analysis finds on memcached |
| `census-dynamic.md` | Which mutexes and `shared_ptr` counts only one thread touches, counted at run time in six programs under two loads each: the ceiling for removing a mutex or an atomic count by proving the object confined |
| `related-work.md` | What the compiler does against where each technique was done before, claim by claim, with checked citations; papers collected earlier are under `related-works/` |
| `declared-topology-audit.md` | Hand-declared queue topologies read in DPDK's examples and in users of four SPSC libraries: which hold, which only by convention, and what a compiler check needs |
| `correctness.md` | Why substituting a queue preserves the program: one lemma per implementation under its precondition, the facts the analyses must prove, and what is assumed rather than derived |
| `paper-outline.md` | The paper's outline around topology and ownership, the experiments it needs with the leg behind each, and the claims no measurement supports yet |
| `toolchain.md` | Building and sharing the ClangIR-enabled Clang the project needs |
| `MLIR Concur.pptx` | Early presentation on the dialect design and motivating examples; an artefact kept verbatim, metadata included |

## What is deliberately not here

Grant applications are not committed to this repository. `vision.md` carries
their technical content with the funding apparatus removed: no funding
roles, no institution, no budget, no staffing, no work packages, no
timeline, no milestones, no impact section. If you need the applications
themselves, they are held outside the repository.

Nothing in this directory should be assumed confidential. Nothing that is
confidential should be added to it.
