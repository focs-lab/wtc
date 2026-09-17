# 2. CI gets the toolchain from a registry image, not from a self-hosted runner

Taken 2026-09-17, reversing a choice made earlier the same day before any of
it was built.

## Context

Decision 1 settled that WTC is out of tree against a pinned llvm-project. This
one is about how the toolchain reaches CI. Nothing can be built without a Clang
configured with `CLANG_ENABLE_CIR=ON`, which no distribution ships, so a
compiler has to be produced once and delivered somewhere.

The first answer was a self-hosted runner on the lab server, on the reasoning
that the machine is already there, the builds are heavy, and a multi-gigabyte
image pulled on every run would cost more than it saves. Both halves of that
reasoning turned out to be wrong.

## What changed the answer

**The security argument is stronger than it first looked.** On a
`pull_request` event from a fork, the workflow that runs is the one in the
merge commit, which the fork author controls. A job condition that restricts
the heavy job to in-repository branches therefore lives in a file the attacker
can edit; what actually holds the line is the approval setting, which is a
person reading a diff for malice under time pressure, every time, forever. And
the residual is worse than the headline: anyone with push access runs code on
the server by definition, so the machine's security becomes the security of a
contributor's GitHub account. No setting closes that.

**The size argument evaporated on measurement.** Built with shared libraries,
one target architecture and no examples, the installed toolchain is 741 MB, not
the several gigabytes assumed. A container around it is under a gigabyte and
pulls in seconds. The recurring CI cost was also overestimated: WTC is a
handful of translation units linking a prebuilt MLIR, which a free hosted
runner finishes in minutes.

With the size objection gone, a self-hosted runner buys nothing and keeps a
permanent execution surface on a shared research machine.

## Decision

CI runs on GitHub-hosted runners. The toolchain is a public image in the GitHub
Container Registry, built on a machine with cores to spare and published once
per pin move.

The image tag is derived from the submodule pin, not configured separately:
`git rev-parse HEAD:llvm` gives the revision, and the workflow refuses to run
if no image exists for it. The image carries the same revision in an
environment variable, which the build job compares against the pin. The
toolchain and the source it was built from cannot drift apart silently.

The Dockerfile compiles nothing. It copies in a tree staged with `DESTDIR`.
Compiling inside the container would put the whole build under the container
runtime's memory cap, which is both smaller than the machine's and the harder
one to recover from when it is exceeded.

## Two flags that are off by default and break testing silently

Both were found by building rather than by reading, and both produce a green
CI that has tested nothing:

- `LLVM_INSTALL_UTILS` defaults to OFF, so `FileCheck`, `not` and `count` are
  never installed. Every test uses `FileCheck`.
- `llvm-lit` has no install rule at all. Without `LLVM_EXTERNAL_LIT` pointing
  at a real one, CMake emits a warning rather than an error and `check-wtc`
  runs zero tests.

The workflow therefore asserts that the suite was not empty. Four lines of YAML
stand between this project and a year of misplaced confidence in a green badge.

## Consequences

Fork pull requests get the full build, which a self-hosted runner could never
have allowed. Nothing runs on the lab server on anyone else's behalf. The cost
is one manual publish step whenever the pin moves, and the workflow makes
forgetting it a loud failure rather than a confusing one.

Revisit if CI time on hosted runners becomes the bottleneck, which would mean
the project grew a great deal, or if the image outgrows a hosted runner's disk.
