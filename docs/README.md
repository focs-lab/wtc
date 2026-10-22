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
| `related-work.md` | What the compiler does against where each technique was done before, claim by claim, with checked citations; papers collected earlier are under `related-works/` |
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
