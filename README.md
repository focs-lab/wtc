# WTC — Well-tempered Compiler for Multithreading

WTC teaches the compiler what a concurrent data structure *is*, so that it can
pick the implementation the program actually needs.

A library queue cannot know that only one thread ever pushes to it. It must
assume the worst and pay for a multi-producer algorithm. WTC lifts calls into a
thin shim library up to first-class operations in an MLIR dialect, proves facts
about who touches each object, and lowers back down to an implementation chosen
for that program rather than for the general case.

## Status

Early. The project builds and its tests pass against the pinned toolchain, but
what exists is one vertical slice and it is not finished:

| Piece | State |
| --- | --- |
| `wtc` dialect: `mutex` and `queue<T>` types, `lock`, `unlock`, `queue_push`, `queue_pop` | defined; no verifiers, no memory-effect or progress semantics yet |
| Lifting CIR calls into the dialect | works on hand-written IR; the C++ path is not yet exercised end to end |
| Queue ownership analysis: SPSC / MPSC / SPMC / MPMC | prints a verdict; known to be wrong for a spawn inside a loop, for the main thread, and for functions taking more than one queue |
| Lowering back to CIR | faithful round trip; crashes on operations that carry no source-type attribute |
| Specialised implementations | none yet |

`docs/audit.md` lists the known defects in full. Do not build anything on top of
the analysis verdict until they are closed.

## Building

WTC is an out-of-tree MLIR project. It needs a Clang built with ClangIR
enabled, which is off by default, so a distribution package will not do. It
relies on `cir::AnnotationAttr` and the `annotations` field of `cir.func`, both
of which are in upstream llvm-project.

The exact revision is pinned as the `llvm/` submodule. Do not use a different
one and expect it to work. The submodule points at `focs-lab/llvm-project-wtc`,
a fork of upstream, on its `wtc/main` branch. That branch carries none of our
own commits yet; it exists so that when it does, everyone can get them. See
`decisions/0001-out-of-tree-with-a-pinned-llvm.md`.

```sh
git clone --recurse-submodules https://github.com/focs-lab/wtc
cd wtc
```

The quickest way in is the prebuilt toolchain image, which is what CI uses:

```sh
# <pin> is the first twelve characters of the pinned llvm revision:
#   git rev-parse HEAD:llvm
docker run --rm -it -v "$PWD:/src" -w /src \
  ghcr.io/focs-lab/wtc-toolchain:llvm-<pin>

cmake -G Ninja -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DMLIR_DIR=/opt/llvm/lib/cmake/mlir \
  -DLLVM_DIR=/opt/llvm/lib/cmake/llvm \
  -DClang_DIR=/opt/llvm/lib/cmake/clang \
  -DLLVM_EXTERNAL_LIT="$(command -v lit)"
cmake --build build
cmake --build build --target check-wtc
```

On Apple Silicon add `--platform linux/amd64`; the image is x86-64 only.

To build the toolchain yourself instead, follow `docs/toolchain.md`. It carries
the exact flags, the measured time, memory and disk it took, and the one
default that has to be overridden or the test suite cannot run at all.

Two things to know either way. Build the toolchain outside `llvm/`, or the
submodule shows up permanently dirty, because the superproject's ignore rules
do not reach inside it. And pick the job count from the machine's memory rather
than its core count.

## Layout

```
llvm/         llvm-project, pinned; a submodule on our fork's wtc/main
mlir/         the dialect, the passes and wtc-opt
shim/         the wtc:: headers a user program includes
runtime/      specialised implementations the compiler substitutes in
benchmarks/   workloads and measurements
test/         lit tests
decisions/    why the project is built the way it is
docs/         motivation, glossary, audit, related work
docker/       the toolchain image CI and contributors build against
tools/        publishing the toolchain, applying the branch policy
.github/      the workflow and the review rules
```

## Contributing

Everything lands through a pull request. `main` takes no direct pushes. Every
change that alters behaviour brings a test; a bug fix brings the test that
would have failed before it. File moves and renames go in their own commit,
never mixed with substantive changes.

`CODEOWNERS` assigns reviewers by directory, on purpose: whoever does not own a
part reviews it.

To change LLVM itself, commit inside `llvm/` on `wtc/main` and push it, then
record the new commit in a pull request here that changes nothing else. The pin
moves on its own, never as a side effect.

## License

Apache License 2.0 with LLVM Exceptions, the same terms as llvm-project, so
that anything worth sending upstream can go upstream without relicensing.
