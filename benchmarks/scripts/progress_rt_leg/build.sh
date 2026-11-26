#!/usr/bin/env bash
# build.sh <tree> <out> <jobs>: the standalone benchmarks of one source tree
# (benchmarks/, runtime/, shim/ of one commit), clang++-18, Release,
# -march=native, and WB (wb_bounded.cpp, beside this script) compiled with
# w9_mpmc_scaling's own compile command against the same tree.
set -eu
T=$(cd "$1" && pwd); O=$2; J=${3:?jobs}
H=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$O"; O=$(cd "$O" && pwd)
cmake -S "$T/benchmarks" -B "$O" -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_C_COMPILER=clang-18 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DWTC_BENCH_MARCH=${WB_MARCH:-native} -DCMAKE_CXX_FLAGS="${WB_CXXFLAGS:-}" > "$O/configure.log"
ninja -C "$O" -j"$J" w1_throughput w9_mpmc_scaling w10_ablation w11_pool wtc_lfring > "$O/build.log"
cmd=$(python3 -c '
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e["file"].endswith("w9_mpmc_scaling.cpp"):
        a = shlex.split(e["command"])
        out = []; skip = False
        for x in a:
            if skip: skip = False; continue
            if x in ("-o", "-c"): skip = x == "-o"; continue
            if x.endswith("w9_mpmc_scaling.cpp"): continue
            out.append(x)
        print(shlex.join(out)); break' "$O/compile_commands.json")
extra=""; grep -q '^ *scq = ' "$T/shim/topology.hpp" && extra="-DWB_HAVE_INDEX=1"
case $cmd in *WTC_HAVE_TBB*) libs=-ltbb;; *) libs=;; esac
eval "$cmd" $extra -I"$T/benchmarks/workloads" -I"$T/shim" "$H/wb_bounded.cpp" -o "$O/wb_bounded" "$O/libwtc_lfring.a" $libs -pthread
echo "wb=$extra" >> "$O/build-info.txt"
echo "built $O"
