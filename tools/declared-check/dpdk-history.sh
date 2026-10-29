#!/usr/bin/env bash
#
# Runs the declared-topology check over the files of DPDK's two historical
# fixes of a wrong mempool declaration (benchmarks/results/
# 2026-10-26-declared-recall/history.tsv), at the parent of each fix and at
# the fix:
#
#   27a26d65f8 app/dumpcap (2023): the capture pool's ops "ring_mp_sc"
#              became "ring_mp_mc"; app/dumpcap/main.c, and
#              lib/pdump/rte_pdump.c, where the primary process takes
#              from that pool (unchanged by the fix)
#   46d3f57537 vhost (2020): MEMPOOL_F_SC_GET dropped from the IOTLB pool;
#              lib/librte_vhost/iotlb.c
#
#   tools/declared-check/dpdk-history.sh <work dir> <dpdk clone> <libpcap include dir>
#
# The clone is only read: each revision's tree is exported with git
# archive into <work dir>/<case>-<before|after> (a blobless clone fetches
# the blobs it needs). Each file is one translation unit, as DPDK's build
# makes it; it is compiled with the pinned ClangIR, as dpdk.sh compiles the
# examples, with the tree's lib/*, its EAL include directories, config, a
# meson build directory configured for the generated rte_build_config.h
# only, and for dumpcap libpcap's headers (pcap/pcap.h; libpcap 1.10.4 was
# used). Every remark goes to <work dir>/<case>-<side>.log; the lines about
# queues are printed.
#
# Environment: CLANG, WTC_OPT as in dpdk.sh; MESON (default: meson, run
# with a python3 that has the modules DPDK's build asks for, pyelftools),
# or BUILD_DUMPCAP and BUILD_VHOST, build directories already configured
# for the two parents (each fix changes one line, and the after side
# reuses its parent's); VACUOUS as in dpdk.sh.
set -euo pipefail

OUT=$(mkdir -p "${1:?usage: $0 <work dir> <dpdk clone> <libpcap include dir>}" && cd "$1" && pwd)
CLONE=$(cd "${2:?}" && pwd)
PCAP=$(cd "${3:?}" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CLANG=${CLANG:-$ROOT/build-llvm/bin/clang}
WTC_OPT=${WTC_OPT:-$ROOT/build/bin/wtc-opt}
MESON=${MESON:-meson}
ANALYSIS=--queue-ownership-analysis
[ "${VACUOUS:-1}" = 0 ] && ANALYSIS=--queue-ownership-analysis=vacuous-outcome=false
FLAGS="-std=gnu11 -O0 -include rte_config.h -D_GNU_SOURCE -DALLOW_EXPERIMENTAL_API -march=x86-64-v2 -w"

export_tree() {  # dir rev
  [ -f "$1/meson.build" ] && return
  mkdir -p "$1"
  git -C "$CLONE" archive --format=tar "$2" | tar -x -C "$1"
}

configure() {  # tree build-dir meson-options...
  local tree=$1 b=$2
  shift 2
  [ -f "$b/rte_build_config.h" ] && return
  # Only the configuration header is needed; a later step of the setup
  # may fail (an old meson's ninja check) once it is written.
  "$MESON" setup "$b" "$tree" "$@" >"$b.meson.log" 2>&1 || true
  [ -f "$b/rte_build_config.h" ] || { echo "meson did not configure $tree; see $b.meson.log" >&2; exit 1; }
}

check() {  # name tree build-dir file [extra flags]
  local name=$1 D=$2 B=$3 f=$4 d
  shift 4
  local inc="-I$B -I$D/config"
  # The EAL's include directories, in the 2020 layout (lib/librte_eal)
  # and the current one (lib/eal), and every library's.
  for d in "$D"/lib/eal/include "$D"/lib/eal/x86/include "$D"/lib/eal/linux/include \
           "$D"/lib/librte_eal/include "$D"/lib/librte_eal/x86/include "$D"/lib/librte_eal/linux/include \
           "$D"/lib/*/; do
    [ -d "$d" ] && inc="$inc -I${d%/}"
  done
  # shellcheck disable=SC2086
  if ! "$CLANG" $FLAGS $inc -I"$(dirname "$D/$f")" "$@" -fclangir -emit-cir "$D/$f" -o "$OUT/$name.cir" \
       2>"$OUT/$name.cir.err"; then
    echo "== $name: no CIR: $(grep -m1 error "$OUT/$name.cir.err")"
    return
  fi
  local rc=0
  "$WTC_OPT" "$OUT/$name.cir" --lift-cir-to-wtc "$ANALYSIS" -o /dev/null >"$OUT/$name.log" 2>&1 || rc=$?
  echo "== $name (wtc-opt exit $rc)"
  grep -E 'wtc: (queue|the address)' "$OUT/$name.log" | sed -E 's#^[^ ]*/##' | sort -u || true
}

export_tree "$OUT/dumpcap-before" 27a26d65f8^
export_tree "$OUT/dumpcap-after" 27a26d65f8
export_tree "$OUT/vhost-before" 46d3f57537^
export_tree "$OUT/vhost-after" 46d3f57537

BD=${BUILD_DUMPCAP:-$OUT/build-dumpcap}
BV=${BUILD_VHOST:-$OUT/build-vhost}
# DPDK 23.11-rc2 and 20.11-rc0 take different option names.
configure "$OUT/dumpcap-before" "$BD" -Dplatform=generic -Dmax_numa_nodes=1 -Ddisable_drivers='*' \
  -Denable_apps= -Dtests=false -Dexamples=
configure "$OUT/vhost-before" "$BV" -Dmachine=default -Dmax_numa_nodes=1 -Ddisable_drivers='*' \
  -Dtests=false -Dexamples=

for side in before after; do
  check "dumpcap-$side" "$OUT/dumpcap-$side" "$BD" app/dumpcap/main.c -I"$PCAP"
  check "pdump-$side" "$OUT/dumpcap-$side" "$BD" lib/pdump/rte_pdump.c
  check "vhost-$side" "$OUT/vhost-$side" "$BV" lib/librte_vhost/iotlb.c
done
