#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Builds the spdlog leg's program (bench.cpp) over spdlog v1.14.1:
#
#   spdlog-orig       spdlog as released, header-only: the mutex and
#                     condition-variable queue (what a program gets today)
#   spdlog-generic    the port, wtc-cc --no-specialize: the shim's
#                     general-purpose queue (B0)
#   spdlog-compiler   the port, wtc-cc: the compiler's choice, made when
#                     the pool is built from its number of workers
#   spdlog-hand       the port with the pool's queue declared mpsc by hand
#                     (B3), which the compiler checks
#
#   build.sh <spdlog-dir> <out-dir>
#
# <spdlog-dir> is what fetch.sh clones; it is only read. Needs clang-18
# and the WTC toolchain built in the tree this script is in. Never run it
# inside a `bench` reservation; it compiles. TSAN=1 adds the compiler's
# build under ThreadSanitizer.
set -euo pipefail
S=$(cd "${1:?spdlog dir}" && pwd)
mkdir -p "${2:?out dir}"; W=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
WTC=$(cd "$HERE/../../.." && pwd)
cd "$W"
mkdir -p bin
python3 "$HERE/port.py" "$S" "$W/include"

INC="-I$WTC/benchmarks/harness -I$WTC/benchmarks/queues"
clang++-18 -O2 -std=c++17 -I"$S/include" $INC "$HERE/bench.cpp" -lpthread -o bin/spdlog-orig

build() { # name, wtc-cc flags...
  local name=$1; shift
  "$WTC/tools/wtc-cc" --toolchain "$WTC/build-llvm" -O2 -I"$W/include" $INC "$@" "$HERE/bench.cpp" -lpthread \
    -o "bin/spdlog-$name" 2>"bin/spdlog-$name.log" || { echo "$name: FAILED"; tail -5 "bin/spdlog-$name.log"; return 1; }
  echo "== $name"
  grep -oE 'wtc: (queue [^:]+: [a-z_ ]+\([^)]*\)|specialised[^;]*|queue access is unresolvable \([^)]*\)|queue [^;]* is declared [^;]*)' "bin/spdlog-$name.log" \
    | sort | uniq -c | sed 's/^/  /' || true
}
build generic  --no-specialize
build compiler
build hand     --no-specialize -DSPDLOG_WTC_HAND_MPSC
if [ "${TSAN:-0}" = 1 ]; then
  build compiler-tsan --tsan -O1 -g
fi
{
  echo "compiler=tools/wtc-cc, ClangIR $(git -c safe.directory="$WTC" -C "$WTC" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-' | cut -c1-12)"
  echo "wtc=$(git -c safe.directory="$WTC" -C "$WTC" rev-parse --short=12 HEAD 2>/dev/null)"
  echo "spdlog=$(git -c safe.directory="$S" -C "$S" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
  echo "orig=clang++-18 -O2 header-only"
} > bin/build-info.txt
