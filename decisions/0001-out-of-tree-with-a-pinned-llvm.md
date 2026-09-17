# 1. Out of tree, against a pinned fork of llvm-project

Taken 2026-09-17.

## A correction worth keeping

An earlier reading of the dependency pointed at the ClangIR incubator,
`github.com/llvm/clangir`, as required, because upstream
`llvm-project` supposedly had no `cir::AnnotationAttr` and no `annotations`
field on `cir.func`. That was wrong. It was checked against a third-party fork
of llvm-project that happened to be sitting on a developer's machine and that
lagged upstream, and the result was reported as though it came from upstream.

Upstream has both: `CIR_AnnotationAttr` with mnemonic `annotation`, alongside
`CIR_AnnotationArrayAttr`, and `OptionalAttr<CIR_AnnotationArrayAttr>:$annotations`
on `CIR_FuncOp`. The incubator is not a dependency.

## Context

WTC needs a Clang that emits CIR and records `__attribute__((annotate))` on
functions, because that is how the lifting pass recognises calls into the shim
library. Upstream provides this behind `-DCLANG_ENABLE_CIR=ON`, which is off by
default, so the compiler is built rather than installed from a package.

Nothing in WTC needs the compiler modified *today*. The dialect, the passes,
the analyses and the runtime library are ordinary out-of-tree code.

But "today" is the wrong horizon. Three things in the plan reach into the
compiler: emitting dialect operations straight out of CIRGen, teaching Clang's
attribute grammar real `[[wtc::...]]` attributes instead of string-valued
`annotate`, and dropping sanitizer instrumentation the analysis has proved
unnecessary. Over the life of this project the chance of needing none of them
is small.

## Decision

Out of tree. `llvm-project` is a git submodule pinned at one exact commit.

The submodule points at `focs-lab/llvm-project-wtc`, a fork of upstream, and
not at `llvm/llvm-project` directly. The reason is collaboration, not mirroring:
a submodule pointing at upstream lets anyone modify LLVM locally, but nobody can
push, so a modification cannot reach a colleague or CI. A commit made in such a
submodule exists on one machine only, and recording it in the superproject
would hand everyone a pointer they cannot fetch.

The organisation already has a `focs-lab/llvm-project`, a clean but stale fork
of upstream belonging to unrelated work. It is deliberately left alone; hence
the distinct name.

Moving the pin is its own pull request with its own review, never a side effect
of another change.

## Branch layout in the fork

Two branches, and the distinction matters:

- `main` mirrors upstream. Nothing of ours is ever committed here. Sync it when
  convenient, or never.
- `wtc/main` is the branch the submodule pins. Today it points at exactly one
  upstream commit with nothing on top, so it costs nothing and changes nothing.
  A patch, when there is one, is a commit on this branch.

Moving to a newer upstream is then a rebase of `wtc/main`, which is an ordinary
and well-understood operation, rather than reapplying files and discovering the
conflicts at build time.

## What "just a submodule" means in practice

`llvm/` is an ordinary git clone on a detached HEAD at the pinned commit. It is
not read-only. Edit files there, build from them, commit, push to `wtc/main`.
Everyone with write access to the organisation can do the same, which is the
whole point of pointing at the fork.

Two things to know while working in there. The superproject reports the
submodule as changed in `git status`, either as new commits or as modified
content, which is the intended signal and not a problem. And `git submodule
update --force` moves the submodule back to the pinned commit, discarding
uncommitted work in it, so commit to a branch before running it.

## Consequences

Cheap today: `wtc/main` carries zero commits, so the fork is a rename of an
upstream commit and nothing more. Not cheap to skip later: adopting a fork
mid-task means re-pointing `.gitmodules` while somebody is blocked, and every
clone re-running `git submodule sync`.

The prototype was written against the incubator, so expect small adjustments
where the passes read annotations: upstream types the array as
`CIR_AnnotationArrayAttr` rather than a plain `ArrayAttr`, which changes what
the generated accessor hands back.

Once `wtc/main` carries its first real commit, write down who moves the pin and
how often, as decision 2. A carried patch with no integration schedule is how a
fork becomes a burden.

Deliberately not setting `submodule.llvm.branch`: it does nothing unless
someone runs `git submodule update --remote`, and that command would silently
float the pin, which is exactly what this decision exists to prevent.
