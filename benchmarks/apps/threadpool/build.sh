#!/usr/bin/env bash
# SPDX-License-Identifier: Zlib
#
# Builds the ThreadPool leg's programs (bench.cpp) over ThreadPool at the
# pinned commit. Two programs, each in several builds:
#
#   threadpool-<build>        the main thread is the one submitter
#   threadpool-multi-<build>  --submitters threads submit (-DTP_SUBMITTERS)
#
#   orig        the library as released, through wtc-cc: the same compiler
#               as every port build, with no queue of its own to change
#   orig-clang  the same, by clang++-18: what a program gets today
#   orig-spin   the control (port.py --orig-spin): the library's queue and
#               mutex, its workers spinning where they slept
#   generic     the port, wtc-cc --no-specialize: the shim's general-purpose
#               queue (B0)
#   compiler    the port, wtc-cc: the compiler's choice
#   hand        the port with the queue declared spmc by hand (B3), which
#               the compiler checks; the single-submitter program only,
#               since with several submitters there is nothing to declare
#
#   build.sh <threadpool-dir> <out-dir>
#
# <threadpool-dir> is what fetch.sh clones; it is only read. Needs clang-18
# and the WTC toolchain built in the tree this script is in. Never run it
# inside a `bench` reservation; it compiles. JOBS (default 4) builds at
# once. TSAN=1 adds the compiler's builds of both programs under
# ThreadSanitizer. Each build's remarks are in bin/<name>.log, and the
# analysis's verdicts are summarised on the standard output.
set -euo pipefail
S=$(cd "${1:?threadpool dir}" && pwd)
mkdir -p "${2:?out dir}"; W=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
WTC=$(cd "$HERE/../../.." && pwd)
JOBS=${JOBS:-4}
cd "$W"
mkdir -p bin
python3 -I "$HERE/port.py" "$S" "$W/port"
python3 -I "$HERE/port.py" "$S" "$W/spin" --orig-spin

INC="-I$WTC/benchmarks/harness"
wtc() { # name, wtc-cc flags...
  local name=$1; shift
  rm -f "bin/$name"
  "$WTC/tools/wtc-cc" --toolchain "$WTC/build-llvm" -O2 $INC "$@" "$HERE/bench.cpp" -lpthread \
    -o "bin/$name" 2>"bin/$name.log" || { echo "$name: FAILED"; tail -5 "bin/$name.log"; return 1; }
}
classic() { # name, clang++ flags...
  local name=$1; shift
  rm -f "bin/$name"
  clang++-18 -O2 -std=c++17 $INC "$@" "$HERE/bench.cpp" -lpthread -o "bin/$name" 2>"bin/$name.log" \
    || { echo "$name: FAILED"; tail -5 "bin/$name.log"; return 1; }
}

builds=(
  "wtc threadpool-orig -I$S"
  "classic threadpool-orig-clang -I$S"
  "wtc threadpool-orig-spin -I$W/spin"
  "wtc threadpool-generic -I$W/port --no-specialize"
  "wtc threadpool-compiler -I$W/port"
  "wtc threadpool-hand -I$W/port --no-specialize -DTHREADPOOL_WTC_HAND_SPMC"
  "wtc threadpool-multi-orig -I$S -DTP_SUBMITTERS"
  "classic threadpool-multi-orig-clang -I$S -DTP_SUBMITTERS"
  "wtc threadpool-multi-orig-spin -I$W/spin -DTP_SUBMITTERS"
  "wtc threadpool-multi-generic -I$W/port --no-specialize -DTP_SUBMITTERS"
  "wtc threadpool-multi-compiler -I$W/port -DTP_SUBMITTERS"
)
if [ "${TSAN:-0}" = 1 ]; then
  builds+=("wtc threadpool-compiler-tsan -I$W/port --tsan -O1 -g"
           "wtc threadpool-multi-compiler-tsan -I$W/port -DTP_SUBMITTERS --tsan -O1 -g")
fi
for b in "${builds[@]}"; do
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n || true; done
  $b &
done
wait
# A failed build leaves no binary (each build removes its own first).
fail=0
for b in "${builds[@]}"; do
  set -- $b
  [ -x "bin/$2" ] || { echo "build.sh: $2 failed" >&2; fail=1; }
done
[ "$fail" = 0 ] || exit 1

# What the analysis said about the pool's queue, build by build.
for log in bin/threadpool-*generic.log bin/threadpool-*compiler.log bin/threadpool-hand.log; do
  echo "== $(basename "$log" .log)"
  grep -oE '(remark|warning|error): wtc: .*' "$log" | sed -E 's/^remark: //' \
    | sort | uniq -c | sed 's/^/  /' || true
done
{
  echo "compiler=tools/wtc-cc, ClangIR $(git -c safe.directory="$WTC" -C "$WTC" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-' | cut -c1-12)"
  echo "wtc=$(git -c safe.directory="$WTC" -C "$WTC" rev-parse --short=12 HEAD 2>/dev/null)"
  echo "threadpool=$(git -c safe.directory="$S" -C "$S" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
  echo "orig-clang=clang++-18 -O2"
} > bin/build-info.txt
