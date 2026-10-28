#!/usr/bin/env bash
#
# One repetition of the cost model's check on W8 (NOTE.md): every
# configuration (threads x level of work) in a fresh random order, and
# within each the three builds of build_w8_flavours.sh's cost mode
# (w8c_plain_L, w8c_off_L, w8c_on_L) back to back in a fresh random order,
# so that each ratio is taken within one repetition. Each run's line gets
# the UTC times it started and ended, so that runs can be matched against
# what else ran on the host. Untimed: run it on processors no other job is
# pinned to, one repetition per hold of the build lock.
#
#   run.sh <build-dir> <runs.jsonl> <rep>
set -euo pipefail
B=$1; OUT=$2; REP=$3
LEVELS=${W8_COST_LEVELS:-"0 20 50 100 200 500"}
THREADS=${W8_THREADS:-"1 2 4 8"}
CPUS=${W8_CPUS:-20,21,22,23,24,25,26,27}
N=${W8_N:-1000000}
ONE=$(mktemp)
trap 'rm -f "$ONE"' EXIT
for cfg in $(for t in $THREADS; do for l in $LEVELS; do echo "$t:$l"; done; done | shuf); do
  t=${cfg%:*}; l=${cfg#*:}
  cpus=$(echo "$CPUS" | cut -d, -f1-"$t")
  for f in $(printf 'plain\noff\non\n' | shuf); do
    : > "$ONE"
    t0=$(date -u +%FT%T.%3NZ)
    "$B/w8c_${f}_$l" --flavour "$f" --threads "$t" --work-ns "$l" --n "$N" --cpus "$cpus" \
      --rep "$REP" --json "$ONE" 2>&1 >/dev/null | tail -1
    t1=$(date -u +%FT%T.%3NZ)
    python3 -c 'import json, sys
r = json.loads(open(sys.argv[1]).read())
r["utc_start"], r["utc_end"] = sys.argv[2], sys.argv[3]
print(json.dumps(r, sort_keys=True))' "$ONE" "$t0" "$t1" >> "$OUT"
  done
done
