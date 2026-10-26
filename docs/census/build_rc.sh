#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Builds the spdlog leg's program for the reference-count census, four ways:
#
#   spdlog-orig-census      spdlog as released, through wtc-cc --census
#                           --no-specialize: every count operation reported
#   spdlog-generic-census   the port, wtc-cc --census --no-specialize
#   spdlog-compiler-census  the port, wtc-cc --census
#   spdlog-orig-hooks       spdlog as released, clang++ with
#                           tools/census/rc_hooks.h: releases only, the way
#                           a program built without wtc-cc is counted; set
#                           beside spdlog-orig-census, it calibrates that
#                           partial count against the full one
#
#   docs/census/build_rc.sh <spdlog-dir> <out-dir>
#
# <spdlog-dir> is what benchmarks/apps/spdlog/fetch.sh clones. Needs
# clang-18 and the WTC toolchain built in this tree.
set -euo pipefail
S=$(cd "${1:?spdlog dir}" && pwd)
mkdir -p "${2:?out dir}"; W=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd); WTC=$(cd "$HERE/../.." && pwd)
APP="$WTC/benchmarks/apps/spdlog"
mkdir -p "$W/bin"
python3 "$APP/port.py" "$S" "$W/include"
INC="-I$WTC/benchmarks/harness -I$WTC/benchmarks/queues"
FP="-O2 -g -fno-omit-frame-pointer"

wtc() { # name, include dir, wtc-cc flags...
  local name=$1 inc=$2; shift 2
  "$WTC/tools/wtc-cc" --toolchain "$WTC/build-llvm" --census $FP -I"$inc" $INC "$@" "$APP/bench.cpp" -lpthread \
    -o "$W/bin/spdlog-$name" 2>"$W/bin/spdlog-$name.log" || { echo "$name: FAILED"; tail -5 "$W/bin/spdlog-$name.log"; return 1; }
  grep -o 'wtc-census: .*' "$W/bin/spdlog-$name.log" | sed "s/^/$name: /"
}
wtc orig-census "$S/include" --no-specialize
wtc generic-census "$W/include" --no-specialize
wtc compiler-census "$W/include"
clang++-18 -std=c++17 $FP -include "$WTC/tools/census/rc_hooks.h" -I"$S/include" $INC "$APP/bench.cpp" -lpthread \
  -o "$W/bin/spdlog-orig-hooks"
echo "built: $W/bin"
