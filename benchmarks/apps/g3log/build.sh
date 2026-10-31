#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
#
# Builds the g3log leg's program (bench.cpp) with g3log's sources, as one
# translation unit (wtc-cc --unity), so that the analysis sees the logger,
# its worker and the program together:
#
#   g3log-orig       the sources as released, clang++-18 -O2, one unit
#   g3log-generic    the same through wtc-cc --no-specialize: the queue
#                    class lifted onto wtc::lifted, the general-purpose
#                    queue (B0)
#   g3log-compiler   the same through wtc-cc: lifted, the compiler's choice
#   g3log-port       the hand port (port.py) through wtc-cc: the queue class
#                    rewritten onto wtc::queue, the compiler's choice
#   g3log-port-mpsc  the hand port with its queue declared mpsc by hand,
#                    which the compiler checks
#
#   build.sh <g3log-dir> <out-dir>
#
# <g3log-dir> is what fetch.sh clones; it is only read. CMake configures
# it once, for the header it generates (generated_definitions.hpp). Needs
# clang-18, cmake and the WTC toolchain built in the tree this script is
# in. It compiles: never inside a timed leg.
set -euo pipefail
S=$(cd "${1:?g3log dir}" && pwd)
mkdir -p "${2:?out dir}"; W=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
WTC=$(cd "$HERE/../../.." && pwd)
. "$WTC/benchmarks/scripts/source_rev.sh"
source_rev "$WTC" || exit 2
cd "$W"
mkdir -p bin
cmake -S "$S" -B "$W/cmake" -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=Release \
  -DADD_G3LOG_UNIT_TEST=OFF -DADD_FATAL_EXAMPLE=OFF -DADD_G3LOG_BENCH_PERFORMANCE=OFF >cmake.log 2>&1 \
  || { echo "cmake failed"; tail -20 cmake.log; exit 1; }
python3 "$HERE/port.py" "$S" "$W/port"

UNIT=()
for f in logworker g3log logmessage logcapture loglevels time filesink crashhandler_unix; do UNIT+=("$S/src/$f.cpp"); done
UNIT+=("$HERE/bench.cpp")
INC=(-I"$S/src" -I"$W/cmake/include")
{ for f in "${UNIT[@]}"; do printf '#include "%s"\n' "$f"; done; } > orig.unity.cpp
clang++-18 -O2 -DNDEBUG -std=c++17 "${INC[@]}" orig.unity.cpp -lpthread -o bin/g3log-orig
build() { # name, wtc-cc flags...
  local name=$1; shift
  "$WTC/tools/wtc-cc" --toolchain "$WTC/build-llvm" -O2 -DNDEBUG "$@" "${INC[@]}" --unity "${UNIT[@]}" -lpthread \
    -o "bin/g3log-$name" 2>"bin/g3log-$name.log" || { echo "$name: FAILED"; tail -5 "bin/g3log-$name.log"; return 1; }
  echo "== $name"
  grep -oE 'wtc: (queue .*: (spsc|spmc|mpsc|mpmc|unknown) \([^)]*\)|specialised.*|queue .* is declared .*|queue access is unresolvable.*)' \
    "bin/g3log-$name.log" | sort | uniq -c | sed 's/^/  /' || true
}
build generic --no-specialize
build compiler
build port -I"$W/port"
build port-mpsc -I"$W/port" -DG3LOG_WTC_TAG=wtc::topology::mpsc
{
  echo "compiler=tools/wtc-cc, ClangIR $(echo "$SOURCE_LLVM_SHA" | cut -c1-12)"
  echo "wtc=$(echo "$SOURCE_SHA" | cut -c1-12)$(case $SOURCE_DIRTY in true) echo ' (dirty)';; unknown) echo ' (dirty: unknown)';; esac)"
  echo "g3log=$(git -c safe.directory="$S" -C "$S" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
  echo "orig=clang++-18 -O2 -DNDEBUG, the same sources as one unit"
} > bin/build-info.txt
