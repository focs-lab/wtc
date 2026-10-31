#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Builds the CTPL leg's program (bench.cpp) over CTPL's ctpl_stl.h:
#
#   ctpl-orig       the library as released, clang++-18 -O2
#   ctpl-generic    the same source through wtc-cc --no-specialize: its
#                   queue class lifted onto wtc::lifted, the general-purpose
#                   queue (B0)
#   ctpl-compiler   the same source through wtc-cc: lifted, and the
#                   compiler's choice of queue
#   ctpl-port       the hand port (port.py) through wtc-cc: the queue class
#                   rewritten onto wtc::queue by hand, the compiler's choice
#
#   build.sh <ctpl-dir> <out-dir>
#
# <ctpl-dir> is what fetch.sh clones; it is only read. Needs clang-18 and
# the WTC toolchain built in the tree this script is in. It compiles:
# never inside a timed leg.
set -euo pipefail
S=$(cd "${1:?ctpl dir}" && pwd)
mkdir -p "${2:?out dir}"; W=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
WTC=$(cd "$HERE/../../.." && pwd)
. "$WTC/benchmarks/scripts/source_rev.sh"
source_rev "$WTC" || exit 2
cd "$W"
mkdir -p bin
python3 "$HERE/port.py" "$S" "$W/port"

clang++-18 -O2 -std=c++17 -I"$S" "$HERE/bench.cpp" -lpthread -o bin/ctpl-orig
build() { # name, include dir, wtc-cc flags...
  local name=$1 inc=$2; shift 2
  "$WTC/tools/wtc-cc" --toolchain "$WTC/build-llvm" -O2 -I"$inc" "$@" "$HERE/bench.cpp" -lpthread \
    -o "bin/ctpl-$name" 2>"bin/ctpl-$name.log" || { echo "$name: FAILED"; tail -5 "bin/ctpl-$name.log"; return 1; }
  echo "== $name"
  grep -oE 'wtc: (queue .*: (spsc|spmc|mpsc|mpmc|unknown) \([^)]*\)|specialised.*|queue .* is declared .*|queue access is unresolvable.*)' \
    "bin/ctpl-$name.log" | sort | uniq -c | sed 's/^/  /' || true
}
build generic  "$S" --no-specialize
build compiler "$S"
build port     "$W/port"
{
  echo "compiler=tools/wtc-cc, ClangIR $(echo "$SOURCE_LLVM_SHA" | cut -c1-12)"
  echo "wtc=$(echo "$SOURCE_SHA" | cut -c1-12)$(case $SOURCE_DIRTY in true) echo ' (dirty)';; unknown) echo ' (dirty: unknown)';; esac)"
  echo "ctpl=$(git -c safe.directory="$S" -C "$S" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
  echo "orig=clang++-18 -O2 header-only"
} > bin/build-info.txt
