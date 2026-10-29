#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Builds the spdlog leg's program (bench.cpp) over spdlog v1.14.1:
#
#   spdlog-orig       spdlog as released, header-only: the mutex and
#                     condition-variable queue (what a program gets today)
#   spdlog-orig-spin  the same queue, its waiters spinning on a published
#                     size instead of sleeping, then taking the mutex,
#                     as the port does (spin_original.py): the control that
#                     separates the queue from the way it waits
#   spdlog-generic    the port, wtc-cc --no-specialize: the shim's
#                     general-purpose queue (B0)
#   spdlog-compiler   the port, wtc-cc: the compiler's choice, made when
#                     the pool is built from its number of workers
#   spdlog-hand       the port with the pool's queue declared mpsc by hand
#                     (B3), which the compiler checks
#   spdlog-hand-cxx,  with LANED=1: the tag declared mpsc, and the laned
#   spdlog-laned-cxx  queue's experimental tag, both built with clang++-18
#                     (see below)
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
python3 "$HERE/spin_original.py" "$S" "$W/include-spin"
clang++-18 -O2 -std=c++17 -I"$W/include-spin" $INC "$HERE/bench.cpp" -lpthread -o bin/spdlog-orig-spin

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
# LANED=1 adds the experiment of relaxing the order between logging
# threads (wtc_q.h, SPDLOG_WTC_HAND_LANED), which the compiler does not
# know, so it is built with clang++-18, and beside it the tag declared
# mpsc built the same way, so that the two differ only in the queue. Both
# wait for the queue to drain before the final flush.
if [ "${LANED:-0}" = 1 ]; then
  for v in hand:-DSPDLOG_WTC_HAND_MPSC laned:"-DSPDLOG_WTC_HAND_LANED -DWTC_LANED_MPSC"; do
    # shellcheck disable=SC2086
    clang++-18 -O2 -std=c++17 -I"$W/include" -I"$WTC/shim" $INC ${v#*:} -DSPDLOG_BENCH_DRAIN \
      "$HERE/bench.cpp" -lpthread -o "bin/spdlog-${v%%:*}-cxx"
  done
fi
if [ "${TSAN:-0}" = 1 ]; then
  build compiler-tsan --tsan -O1 -g
fi
{
  echo "compiler=tools/wtc-cc, ClangIR $(git -c safe.directory="$WTC" -C "$WTC" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-' | cut -c1-12)"
  echo "wtc=$(git -c safe.directory="$WTC" -C "$WTC" rev-parse --short=12 HEAD 2>/dev/null)"
  echo "spdlog=$(git -c safe.directory="$S" -C "$S" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
  echo "orig=clang++-18 -O2 header-only"
} > bin/build-info.txt
