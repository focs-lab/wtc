# WTC — Well-tempered Compiler for Multithreading

WTC teaches the compiler what a concurrent data structure *is*, so that it can
pick the implementation the program actually needs.

A library queue cannot know that only one thread ever pushes to it. It must
assume the worst and pay for a multi-producer algorithm. WTC lifts calls into a
thin shim library up to first-class operations in an MLIR dialect, proves facts
about who touches each object, and lowers back down to an implementation chosen
for that program rather than for the general case.

## Status

Early. What exists today is one vertical slice, and it is not finished:

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

# 1. Build Clang, LLVM and MLIR with ClangIR enabled. Hours on a laptop,
#    minutes on a machine with many cores. Prefer the prebuilt toolchain
#    image if you have access to it; see docs/toolchain.md.
cmake -G Ninja -S llvm/llvm -B build-llvm \
  -DLLVM_ENABLE_PROJECTS="clang;mlir" \
  -DCLANG_ENABLE_CIR=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_CCACHE_BUILD=ON
cmake --build build-llvm

# 2. Build WTC against it.
cmake -G Ninja -S . -B build \
  -DMLIR_DIR=$PWD/build-llvm/lib/cmake/mlir \
  -DLLVM_DIR=$PWD/build-llvm/lib/cmake/llvm \
  -DClang_DIR=$PWD/build-llvm/lib/cmake/clang
cmake --build build

# 3. Run the tests.
cmake --build build --target check-wtc
```

Two things about that first step. Build outside `llvm/`, as above, or the
submodule shows up permanently dirty. And pick the job count deliberately: a
Clang build with assertions needs roughly 2.5 GiB per job, so memory bounds it
before core count does.

Budget the disk too. The checkout is a couple of gigabytes and a Release build
with assertions is tens of gigabytes more. A Debug build of Clang and MLIR is
far larger again and is rarely what you want here.

## Layout

```
llvm/         llvm-project, pinned; a submodule on our fork's wtc/main
build-llvm/   where LLVM is built; ignored, never inside llvm/
mlir/         the dialect, the passes and wtc-opt
shim/         the wtc:: headers a user program includes
runtime/      specialised implementations the compiler substitutes in
benchmarks/   workloads and measurements
test/         lit tests
decisions/    why the project is built the way it is
docs/         motivation, glossary, audit, related work
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
