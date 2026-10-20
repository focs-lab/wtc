#!/usr/bin/env bash
#
# The table of reach and precision (RQ1): builds the examples and the
# compiler-built workloads with wtc-cc, takes the build logs of the ported
# applications, keeps the analysis's remarks, and compares its verdicts with
# the oracle (benchmarks/reach/oracle.json).
#
#   reach.sh <out-dir> <ferret-out-dir> <dedup-out-dir>
#
# The application directories are the out-dirs of
# benchmarks/apps/{ferret,dedup}/build.sh, whose bin/ holds a log per build.
# Writes <out-dir>/logs/ (the remarks of each build) and <out-dir>/TABLE.md,
# and fails if any verdict is wrong. Build the applications from the same
# commit first; WTC_SHA names it where the tree is not a git checkout. It
# compiles: never inside a `bench` reservation.
set -euo pipefail
OUT=${1:?out dir}; FERRET=${2:?ferret build dir}; DEDUP=${3:?dedup build dir}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT/logs"; OUT=$(cd "$OUT" && pwd)
INC="-I$ROOT/shim -I$ROOT/benchmarks/queues -I$ROOT/benchmarks/harness"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

keep() { grep -E 'wtc: ' "$1" | sed -E 's/^.*(remark|warning|error): //' > "$2" || true; }

for src in "$ROOT"/examples/*.cpp "$ROOT/benchmarks/workloads/w3_pipeline_wtc.cpp" \
           "$ROOT/benchmarks/workloads/w7_batch_wtc.cpp"; do
  name=$(basename "${src%.cpp}")
  "$ROOT/tools/wtc-cc" --toolchain "$ROOT/build-llvm" $INC -O2 "$src" -o "$TMP/$name" 2>"$TMP/$name.log" \
    || echo "$name: build failed" >&2
  keep "$TMP/$name.log" "$OUT/logs/$name.log"
done
for f in "$FERRET"/bin/ferret-*.log "$DEDUP"/bin/dedup-*.log; do
  [ -f "$f" ] && keep "$f" "$OUT/logs/$(basename "$f")"
done
{
  echo "# Reach and precision"
  echo
  echo "Every queue of every program compiled, the analysis's verdict against the topology read"
  echo "from the source (benchmarks/reach/oracle.json). wtc ${WTC_SHA:-$(git -c safe.directory="$ROOT" -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null)}."
  echo
  python3 "$HERE/reach_table.py" "$ROOT/benchmarks/reach/oracle.json" "$OUT/logs" || true
} > "$OUT/TABLE.md"
cat "$OUT/TABLE.md"
# A wrong verdict fails the run, after the table that shows it.
grep -q ', 0 wrong' "$OUT/TABLE.md"
