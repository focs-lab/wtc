# Related work

Papers collected while shaping the project. The files themselves are under
`related-works/`.

Entries marked *citation incomplete* need their full bibliographic details
filled in before anything cites them in writing.

## Compilers, MLIR and dialect design

- **Fehr, Fan, Pompougnac, Regehr, Grosser. First-Class Verification Dialects
  for MLIR. PLDI 2025.** Makes formal semantics a first-class citizen in MLIR
  so that verification tools can be written once and reused across dialects, by
  lowering a dialect to semantic dialects that map to SMT. Directly relevant to
  giving the `wtc` dialect semantics rather than only syntax.
  `related-works/fehr2025-first-class.pdf`
- **Lucke et al., 2025, on the transform dialect.** Expressing transformation
  schedules as IR rather than as hardcoded pass pipelines.
  `related-works/lucke2025-transform-dialect.pdf`
- **Composable and Modular Code Generation in MLIR.** *Citation incomplete.*
  `related-works/Dialects and other applications/`
- *Citation incomplete*: `related-works/Dialects and other applications/3469030.pdf`
- *Citation incomplete*: `related-works/Compiler and Concurrency/cpe.7935.pdf`

## Program synthesis

- **Solar-Lezama et al. Sketching concurrent data structures. ASPLOS 2008.**
  Synthesising synchronisation from a partial program plus a specification. The
  ancestor of this line of work, and the reason to be careful about scaling
  claims: sketching struggled on production-sized structures.
  `related-works/Synthesizing/solar-lezama2008-sketching.pdf`

## Safe memory reclamation

- **Brown, on epoch-based reclamation.** *Citation incomplete.*
  `related-works/SMR/brown-ebr.pdf`

## Universal constructions and wait-freedom

- **Petrank et al., 2017. Practical wait-free normalized constructions.**
  *Citation incomplete.* `related-works/Universal constructions and etc/`
- **Correia et al. Wait-free universal constructions.** *Citation incomplete.*
  `related-works/Universal constructions and etc/`

## Composition and transactional structures

- **Zhang et al., 2018. Lock-free transactional transformation.**
- **Laborde et al., 2019. Wait-free dynamic transactions.**
- **Elizarov et al., 2019. LOFT.**
- **Cai et al., 2023. Transactional data structures.**

  All *citation incomplete*, under `related-works/Composition, transactions, etc/`.
  Relevant to composing operations across structures, which none of the
  transformations here currently attempt.

## Concurrency bugs and detection

- **Hong et al., 2014. A survey of race bug detection techniques for
  multithreaded programmes.** Software Testing, Verification and Reliability.
  `related-works/concurrency bugs/`
- **A study of concurrency bugs.** *Citation incomplete.*
  `related-works/Surveys/Concurrency-Bugs-Study.pdf`
- *Citation incomplete*: `related-works/Surveys/s13174-017-0055-2.pdf`

## Message passing

- **Mathur, 2025. The complexity of ...** *Citation incomplete.*
  `related-works/Message-passing systems/mathur2025-the-complexity.pdf`
