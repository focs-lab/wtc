#!/usr/bin/env bash
#
# Runs the declared-topology check over the DPDK example applications that
# docs/declared-topology-audit.md reads by hand (part 3 there).
#
#   tools/declared-check/dpdk.sh <work dir> [<dpdk checkout>]
#
# The audit read DPDK at 6bbb7b3 (26.11.0-rc0). A checkout given is used
# only if it is at that commit, and is not written to; without one, DPDK is
# cloned into <work dir> at that commit. Everything the run makes goes to
# <work dir>: the meson build directory (configured once, for DPDK's
# generated rte_build_config.h only; nothing of DPDK is compiled), each
# application's one translation unit (its .c files included into one, in
# the order listed below), its CIR, and every remark in <app>.log; the
# lines about rings are printed.
#
# Environment: CLANG (ClangIR's clang, default: the pinned build's),
# WTC_OPT (default: build/bin/wtc-opt), MESON (default: meson). For a
# modified copy of the examples (benchmarks/scripts/declared_recall.sh):
# EXAMPLES (the examples directory to read, default: the checkout's),
# DPDK_BUILD (an already configured build directory, default: <work
# dir>/dpdk-build) and ONLY (one application's name, default: all).
# A declaration that holds only because a side it declares single has no
# access here is reported as vacuously satisfied; VACUOUS=0 runs the
# analysis with vacuous-outcome=false, which calls it verified, as runs
# before 29 Oct did.
set -euo pipefail

COMMIT=6bbb7b38b17f9ea4c981b5d374612a16833df41e
OUT=$(mkdir -p "${1:?usage: $0 <work dir> [<dpdk checkout>]}" && cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CLANG=${CLANG:-$ROOT/build-llvm/bin/clang}
WTC_OPT=${WTC_OPT:-$ROOT/build/bin/wtc-opt}
MESON=${MESON:-meson}
ANALYSIS=--queue-ownership-analysis
[ "${VACUOUS:-1}" = 0 ] && ANALYSIS=--queue-ownership-analysis=vacuous-outcome=false

if [ -n "${2:-}" ]; then
  D=$(cd "$2" && pwd)
else
  D=$OUT/dpdk
  [ -d "$D/.git" ] || git clone -q https://github.com/DPDK/dpdk.git "$D"
  git -C "$D" checkout -q "$COMMIT"
fi
if [ "$(git -C "$D" rev-parse HEAD)" != "$COMMIT" ]; then
  echo "$D is not at $COMMIT, the commit the audit read" >&2
  exit 1
fi

EX=$(cd "${EXAMPLES:-$D/examples}" && pwd)
B=${DPDK_BUILD:-$OUT/dpdk-build}
if [ ! -f "$B/rte_build_config.h" ]; then
  # No NUMA library and no drivers: only the configuration header matters.
  "$MESON" setup "$B" "$D" -Dplatform=generic -Dmax_numa_nodes=1 \
    -Ddisable_drivers='*' -Denable_apps= -Dtests=false -Dexamples= >"$OUT/meson.log"
fi

INC="-I$B -I$D/config -I$EX/common"
for d in "$D"/lib/eal/include "$D"/lib/eal/x86/include "$D"/lib/eal/linux/include "$D"/lib/*/; do
  INC="$INC -I${d%/}"
done
FLAGS="-std=gnu11 -O0 -include rte_config.h -D_GNU_SOURCE -DALLOW_EXPERIMENTAL_API -march=x86-64-v2 -w"

# simple_mp's command table is generated at build time; generate it as its
# meson.build does.
python3 "$D/buildtools/dpdk-cmdline-gen.py" --context-name=simple_mp_ctx \
  "$D/examples/multi_process/simple_mp/commands.list" >"$OUT/commands.h"

run() {  # name dir files...
  local name=$1 dir=$2 f
  shift 2
  [ -n "${ONLY:-}" ] && [ "$ONLY" != "$name" ] && return
  local tu=$OUT/$name.tu.c
  : >"$tu"
  for f in "$@"; do
    # One static in qos_sched's args.c shares its name with init.c's global.
    [ "$name/$f" = qos_sched/args.c ] && echo '#define app_numa_mask args_app_numa_mask' >>"$tu"
    echo "#include \"$EX/$dir/$f\"" >>"$tu"
    [ "$name/$f" = qos_sched/args.c ] && echo '#undef app_numa_mask' >>"$tu"
  done
  # shellcheck disable=SC2086
  if ! "$CLANG" $FLAGS $INC -I"$EX/$dir" -I"$EX/$dir/shared" -I"$OUT" \
       -fclangir -emit-cir "$tu" -o "$OUT/$name.cir" 2>"$OUT/$name.cir.err"; then
    echo "== $name: no CIR: $(grep -m1 error "$OUT/$name.cir.err")"
    return
  fi
  local rc=0
  "$WTC_OPT" "$OUT/$name.cir" --lift-cir-to-wtc "$ANALYSIS" -o /dev/null \
    >"$OUT/$name.log" 2>&1 || rc=$?
  echo "== $name (wtc-opt exit $rc)"
  grep -E 'wtc: (queue|the address)' "$OUT/$name.log" | sed -E 's#^[^ ]*/##' | sort -u || true
}

cd "$EX"
run packet_ordering packet_ordering main.c
run distributor distributor main.c
run dma dma dmafwd.c
run bbdev_app bbdev_app main.c
run simple_mp multi_process/simple_mp main.c mp_commands.c
run qos_sched qos_sched $(cd qos_sched && ls ./*.c | sed 's#^\./##')
run efd_server server_node_efd efd_server/main.c efd_server/init.c efd_server/args.c
run efd_node server_node_efd efd_node/node.c
run mp_server multi_process/client_server_mp mp_server/main.c mp_server/init.c mp_server/args.c
run mp_client multi_process/client_server_mp mp_client/client.c
run ip_pipeline ip_pipeline $(cd ip_pipeline && ls ./*.c | sed 's#^\./##')
run pipeline pipeline $(cd pipeline && ls ./*.c | sed 's#^\./##')
