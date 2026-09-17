# The toolchain

WTC needs a Clang built from the pinned upstream revision with
`-DCLANG_ENABLE_CIR=ON`, which turns on ClangIR. A distribution package will
not do: the flag is off by default everywhere.

## Build it once, share it

Building this is hours on a laptop and minutes on a machine with many cores.
Build it on the large machine, publish the install tree, and let everyone else
and CI consume the result. This is the single highest-leverage piece of
infrastructure in the project, because without it the only person who can build
WTC is whoever last built a compiler.

```sh
cmake -G Ninja -S llvm/llvm -B build-llvm \
  -DLLVM_ENABLE_PROJECTS="clang;mlir" \
  -DCLANG_ENABLE_CIR=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_CCACHE_BUILD=ON \
  -DCMAKE_INSTALL_PREFIX=/opt/llvm
cmake --build build-llvm -j <jobs>
cmake --install build-llvm
```

Build into `build-llvm` at the top level, not into `llvm/`. Anything written
inside the submodule leaves it permanently dirty, because the superproject's
ignore rules do not reach in there.

## Choosing the job count

Memory bounds this before cores do. A Clang build with assertions needs roughly
2.5 GiB per parallel job, so the job count is the smaller of what the cores
allow and what memory allows. Inside a container the memory limit is the
container's, not the machine's, and exceeding it does not fail cleanly: the
compiles stall, the daemon stops answering, and recovery needs root.

Work it out for the machine you are on rather than letting the build default to
one job per hardware thread, which is wrong on any large machine.

Disk is the other limit. A Release build of Clang and MLIR with assertions runs
to tens of gigabytes, and ccache wants room of its own. Check free space before
starting rather than after filling the filesystem.

## Publishing it

Package `/opt/llvm` as a container image or a tarball, tag it with the pinned
llvm-project revision so that the toolchain and the submodule cannot drift apart,
and push it where both the team and CI can pull it.

Then set the repository variable `WTC_TOOLCHAIN_IMAGE` to that image. The build
job in CI is skipped while that variable is empty and starts running as soon as
it is set.

Rebuild only when the pin moves, which is its own pull request.
