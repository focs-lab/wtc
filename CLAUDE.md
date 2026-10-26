# Working in this repository

Rules for anyone who changes this repository. They hold for every branch.

This file changes only with approval: the exact change is shown first and the file is edited
only after an explicit yes, rules asked for in passing included.

## Language

Everything committed is in English: code, comments, identifiers, file names, commit messages,
pull request titles and bodies, documentation, result notes. Other languages belong in
conversation and in notes kept out of version control.

## Commits are always squashed into logical steps

A branch's history is a series of complete logical steps, always: one commit per self-contained
change, with "fix typo", "address review", "actually fix it" folded into the commit they repair.
Squash before every push, and say in the pull request what was folded into what.

A fix found after a branch was pushed is folded in as well: rewrite, force-push with
`--force-with-lease`, and rebase the branches stacked on it, rather than adding the fix on top.
Ask first only for `main`, or for a branch someone else has commits on or builds on.

The reason is bisectability: `git bisect` works only if every commit builds and passes its
tests. Before a branch is pushed, every commit on it is checked: it configures, builds and passes
`ninja -C build check-wtc`; the runtime's tests pass plain, under ThreadSanitizer and with
`WTC_CHECK` (`runtime/test`, see `runtime/README.md`) where `runtime/` changed; the benchmarks
build where `benchmarks/` or `shim/` changed.

Form, as in the Linux kernel, Git's SubmittingPatches and LLVM: a lowercase subsystem prefix
(`mlir`, `analysis`, `runtime`, `shim`, `benchmarks`, `tools`, `docs`, `decisions`), an
imperative subject, capitalised, no full stop, about 50 characters; a blank line; a body wrapped
at 72 that says why, since the diff says what.

    runtime: Add a segmented queue for many producers and consumers

## Authorship

Commit messages and pull request descriptions carry no trailers or footers crediting the tool
they were written with, under any name. Authorship is git's author field.

## No names

Committed files and commit messages name no person and no role, and no user name or home path
of anyone on the machines the work runs on. Attribute a decision by its date ("decided 21 Oct"),
an idea by its content, literature by title or tool. A paper's own author list and
bibliography, LICENSE and copyright headers, and verbatim upstream text are exempt.

A leg's result files record command lines and directories; `benchmarks/scripts/campaign.sh`
runs `scrub_paths.sh` on them, and anything committed under `benchmarks/results/` must have
been through it.

## Results

- `campaign.sh` names a leg's directory by the UTC date, so a rerun on the same UTC day
  overwrites the earlier run. Copy the directory out of the tree before a rerun.
- Every leg directory that is committed has a `NOTE.md` saying what the leg shows and how far it
  can be trusted. A result named in the README, `docs/evaluation.md` or a note is a median from
  a committed leg.
- The README is the project's showcase: it is updated with every new result or merged feature,
  and its figures are redrawn with `benchmarks/scripts/readme_figures.py`.
- A timed leg needs the whole host quiet for its length, not only its pinned processors: the
  others share the cache, the memory bandwidth and the power budget. It is announced with a
  start and an end, and nothing builds anywhere on the host in between.
