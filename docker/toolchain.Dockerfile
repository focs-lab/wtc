# The toolchain WTC builds against: Clang, LLVM and MLIR with ClangIR enabled.
#
# This image compiles nothing. The toolchain is built on a machine with cores
# to spare and staged with DESTDIR, and this file only copies the result in.
# That keeps the whole build out of the container runtime's memory cgroup,
# which is the failure mode that costs the most to recover from.
#
# Build it as described in docs/toolchain.md; the context is the staging
# directory, not the source tree.

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git python3 python3-pip ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY opt/llvm /opt/llvm

# lit has no install rule in llvm-project, so it is carried in from the source
# tree and installed here. Without it, check-wtc silently runs zero tests.
RUN python3 -m pip install --break-system-packages /opt/llvm/lit-src

# The revision this toolchain was built from. CI compares it against the
# llvm submodule pin and refuses to run if they disagree.
ARG LLVM_REV
ENV WTC_LLVM_REV=${LLVM_REV}

ENV PATH=/opt/llvm/bin:$PATH
ENV LD_LIBRARY_PATH=/opt/llvm/lib
