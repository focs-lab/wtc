#!/usr/bin/env bash
#
# Runs the declared-topology check over supercollider's supernova sndfile
# backend, the one library user of docs/declared-topology-audit.md part 2
# that builds as a translation unit with its own sources and vendored
# libraries.
#
#   tools/declared-check/supercollider.sh <work dir>
#
# Clones supercollider at the audited commit with the two submodules the
# backend needs, then compiles supercollider_driver.cpp to CIR and runs the
# analysis. CLANG and WTC_OPT as in dpdk.sh.
set -euo pipefail

W=$(mkdir -p "${1:?usage: $0 <work dir>}" && cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CLANG=${CLANG:-$ROOT/build-llvm/bin/clang}
WTC_OPT=${WTC_OPT:-$ROOT/build/bin/wtc-opt}
S=$W/supercollider

if [ ! -d "$S" ]; then
  git clone -q https://github.com/supercollider/supercollider.git "$S"
  git -C "$S" checkout -q b2f6e4422088
  git -C "$S" submodule update -q --init --depth 1 external_libraries/nova-tt external_libraries/nova-simd
fi
"${CLANG}++" -std=c++17 -w -I"$S/server/supernova" -I"$S/common" -I"$S/include/common" \
  -I"$S/include/plugin_interface" -I"$S/include/server" -I"$S/external_libraries/boost" \
  -I"$S/external_libraries/boost_sync/include" -I"$S/external_libraries/nova-tt" -I"$S/external_libraries" \
  -I"$S/external_libraries/libsndfile" -fclangir -emit-cir "$HERE/supercollider_driver.cpp" -o "$W/supercollider.cir"
rc=0
"$WTC_OPT" "$W/supercollider.cir" --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null \
  >"$W/supercollider.log" 2>&1 || rc=$?
echo "== supercollider (wtc-opt exit $rc)"
grep -E 'wtc: ' "$W/supercollider.log" | sed -E 's#^[^ ]*/##' | sort -u || true
