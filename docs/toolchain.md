# The toolchain

WTC needs a Clang built from the pinned upstream revision with
`-DCLANG_ENABLE_CIR=ON`, which turns on ClangIR. The flag is off by default
everywhere, so no distribution package will do.

Build it once on a machine with cores to spare, publish it as a container
image, and let everyone else and CI consume that. This is the highest-leverage
piece of infrastructure in the project: without it the only person who can
build WTC is whoever last built a compiler.

## Building it

```sh
cd <repo>
git submodule update --init --recursive

cmake -G Ninja -S llvm/llvm -B build-llvm \
  -DLLVM_ENABLE_PROJECTS="clang;mlir" \
  -DCLANG_ENABLE_CIR=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DBUILD_SHARED_LIBS=ON \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_INSTALL_UTILS=ON \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_PARALLEL_LINK_JOBS=8 \
  -DLLVM_CCACHE_BUILD=ON \
  -DLLVM_CCACHE_DIR=$HOME/.cache/ccache-llvm \
  -DLLVM_CCACHE_MAXSIZE=60G \
  -DCMAKE_INSTALL_PREFIX=/opt/llvm

cmake --build build-llvm -j<jobs>
```

Four of those flags are easy to leave out and each costs something, but only
the first breaks testing.

`LLVM_INSTALL_UTILS` is **off by default**, and without it `FileCheck`, `not`
and `count` are never installed. Every test in this repository uses
`FileCheck`, and lit registers it as a fatal requirement, so the suite fails
loudly rather than skipping quietly. Loudly is the good case; it still means
nobody can run a test.

`BUILD_SHARED_LIBS` is what makes the result small enough to ship. Static
archives, and every tool relinking them, are the bulk of a default install.

`LLVM_TARGETS_TO_BUILD=X86` removes roughly the code generation WTC never
reaches. Set it to whatever the deployment actually targets.

A separate `LLVM_CCACHE_DIR` keeps this build from evicting every other
project's entries out of a shared cache. Check `ccache -p | grep max_size`
before deciding it does not matter.

### Measured on one 112-thread, 250 GB host, at `-j32`

| | |
| --- | --- |
| Targets | 8138 |
| Wall time | 3 h 18 min |
| Peak memory | 15.9 GiB |
| Build tree | 2.7 GB |
| Installed, stripped | 741 MB |

The peak is the number to size future runs from, and it is far below the
2.5 GiB per job that a static build needs: shared libraries make linking much
cheaper. Re-measure after any flag change rather than carrying these forward.

Pick the job count from the machine, not from the core count. Memory bounds it
before cores do, and on a shared host the ceiling is whatever the cgroup
allows minus what is already resident. If the host has a mechanism for
serialising heavy jobs, use it; an unannounced multi-hour build on a shared
machine is how neighbours lose an afternoon.

## Staging the install

```sh
DESTDIR=$PWD/stage cmake --install build-llvm --strip
cp -a llvm/llvm/utils/lit stage/opt/llvm/lit-src
du -sh stage/opt/llvm
```

`DESTDIR` matters: the CMake package files and library search paths are baked
for `/opt/llvm`, which is where the image puts them, while nothing on the build
host needs root.

`llvm-lit` has **no install rule** in llvm-project, so lit is carried in from
the source tree and installed into the image with pip. Skip this and CMake
warns rather than fails, and `check-wtc` then fails on a missing command.

## Publishing the image

```sh
REV=$(git rev-parse HEAD:llvm)
IMG=ghcr.io/<org>/wtc-toolchain:llvm-${REV:0:12}

docker build -f docker/toolchain.Dockerfile --build-arg LLVM_REV="$REV" \
             -t "$IMG" stage

docker run --rm "$IMG" FileCheck --version
docker run --rm "$IMG" lit --version
docker run --rm "$IMG" sh -c 'ls /opt/llvm/lib/cmake/{llvm,mlir,clang} >/dev/null && echo ok'

gh auth refresh -h github.com -s write:packages
gh auth token | docker login ghcr.io -u "$(gh api user -q .login)" --password-stdin
docker push "$IMG"
```

### The package stays private

This organization forbids public and internal packages, so the only available
visibility is private. That is not a problem: CI authenticates. Both jobs that
touch the registry sign in with the token GitHub mints for the job itself, so
there is no long-lived credential in a secret anywhere.

What a private package does need is **one grant, once**. A package published
from a laptop with a personal token belongs to nobody in particular, and a
workflow token is refused. Grant this repository read access:

```
https://github.com/orgs/focs-lab/packages/container/wtc-toolchain/settings
  Manage Actions access -> Add repository -> wtc -> Role: Read
```

The `org.opencontainers.image.source` label in the Dockerfile links the package
to the repository, which is what gives the registry a repository to inherit
permissions from. The label is necessary and not sufficient; the grant above is
the part that actually opens the door, and it survives every later push.

Private packages count against the organization's storage quota, where public
ones are free. Transfer into GitHub Actions is free either way, so the pull
itself costs nothing. The size is worth knowing before the first surprise:

| image | size |
|---|---|
| uncompressed, as it lands in the runner | 1.3 GiB |
| compressed, as the registry stores it | 377 MiB |

One tag per llvm pin, so two pins already approach the smallest free quota.
**Delete the old version when the pin moves**, in the same settings page.

Point CI at it, image name only, no tag. The workflow derives the tag
from the submodule pin, so the two cannot drift:

```sh
gh variable set WTC_TOOLCHAIN_IMAGE -R <org>/wtc -b 'ghcr.io/<org>/wtc-toolchain'
```

Rebuild and republish only when the pin moves, which is its own pull request.
Moving the pin without publishing an image makes CI fail with a message saying
exactly that.

## Building WTC against it

```sh
cmake -G Ninja -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DMLIR_DIR=/opt/llvm/lib/cmake/mlir \
  -DLLVM_DIR=/opt/llvm/lib/cmake/llvm \
  -DClang_DIR=/opt/llvm/lib/cmake/clang \
  -DLLVM_EXTERNAL_LIT="$(command -v lit)"
cmake --build build
cmake --build build --target check-wtc
```

`LLVM_EXTERNAL_LIT` is required and easy to get wrong. `llvm-lit` has no
install rule in llvm-project, so an install tree does not ship one, and an
empty value is indistinguishable from an unset one: CMake falls back to a path
that does not exist, warns once, and the failure surfaces much later. If `lit`
is not on `PATH`, `$(command -v lit)` expands to nothing and you get exactly
that. The CI workflow therefore checks for `lit` before configuring.

Nothing needs to be said about library search paths. The test configuration
passes only `HOME`, `INCLUDE`, `LIB`, `TMP` and `TEMP` through, so
`LD_LIBRARY_PATH` never reaches `wtc-opt` under lit; the project's own
`CMakeLists.txt` handles this by setting an install rpath built from
`LLVM_LIBRARY_DIR`, which is correct both against a build tree and against an
installed one. Passing `-DCMAKE_BUILD_RPATH` on the command line, which this
document used to recommend, defeats that.

Against a local build tree rather than the image, point the three `_DIR`
variables at `build-llvm/lib/cmake/...` instead. Nothing else changes.
