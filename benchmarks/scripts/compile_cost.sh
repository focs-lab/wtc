#!/usr/bin/env bash
#
# What the pipeline costs at compile time (RQ5): for each source, the wall
# time of each wtc-cc step and of each wtc-opt pass, against the same clang
# compiling the same source the ordinary way, and the size of the code it
# produced with the substitutions and without them.
#
#   compile_cost.sh <out-dir> [reps] [source ...]
#
# Default sources: the examples and the compiler-built workloads. Each
# source is built REPS times (10 by default) and the medians reported;
# runs.jsonl has every run. Compile times are not a timed leg's numbers:
# this needs no reservation, only a machine whose other work is steady,
# and it says so in its summary. Never run inside a `bench` reservation;
# it compiles.
set -euo pipefail

OUT=${1:?output directory}; REPS=${2:-10}; shift $(( $# >= 2 ? 2 : $# ))
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
RUNS="$OUT/runs.jsonl"; : > "$RUNS"
CLANGXX="$ROOT/build-llvm/bin/clang++"
export WTC_OPT=${WTC_OPT:-$ROOT/build/bin/wtc-opt}
INC="-I$ROOT/shim -I$ROOT/benchmarks/queues -I$ROOT/benchmarks/harness"

SOURCES=("$@")
if [ ${#SOURCES[@]} -eq 0 ]; then
  SOURCES=("$ROOT"/examples/*.cpp "$ROOT/benchmarks/workloads/w3_pipeline_wtc.cpp"
           "$ROOT/benchmarks/workloads/w7_batch_wtc.cpp" "$ROOT/benchmarks/workloads/w8_lock_batch_wtc.cpp")
fi

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for rep in $(seq 0 $((REPS-1))); do
  for src in "${SOURCES[@]}"; do
    name=$(basename "${src%.*}")
    : > "$TMP/times"
    WTC_CC_TIMES="$TMP/times" "$ROOT/tools/wtc-cc" --toolchain "$ROOT/build-llvm" $INC -O2 \
      --wtc-opt-arg --mlir-timing --wtc-opt-arg --mlir-timing-display=list "$src" -o "$TMP/wtc.exe" \
      2>"$TMP/timing.txt" || { echo "$name: wtc-cc failed" >&2; continue; }
    # The general build is only for its code size, and some sources are
    # meant not to build that way (a lock-free promise the general queue
    # breaks); a missing one is left out, not fatal.
    rm -f "$TMP/generic.exe"
    "$ROOT/tools/wtc-cc" --toolchain "$ROOT/build-llvm" $INC -O2 --no-specialize "$src" -o "$TMP/generic.exe" \
      2>/dev/null || true
    t0=$(date +%s%N)
    "$CLANGXX" -std=c++17 $INC -O2 "$src" -o "$TMP/plain.exe" -pthread 2>/dev/null \
      || { echo "$name: clang failed" >&2; continue; }
    t1=$(date +%s%N)
    python3 - "$name" "$rep" "$TMP" "$(( t1 - t0 ))" >> "$RUNS" <<'EOF'
import json, re, subprocess, sys
name, rep, tmp, plain_ns = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
r = {"workload": "compile", "variant": name, "rep": rep, "plain_s": plain_ns / 1e9}
for line in open(f"{tmp}/times"):
    step, secs = line.split()
    r[step.replace("-", "_") + "_s"] = float(secs)
r["wtc_cc_s"] = sum(v for k, v in r.items() if k in ("cir_s", "wtc_opt_s", "cir_translate_s", "backend_s"))
# --mlir-timing-display=list: "  0.0012 ( 3.4%)  PassName" per pass
for line in open(f"{tmp}/timing.txt"):
    m = re.match(r"\s*([0-9.]+) \(\s*[0-9.]+%\)\s+(.+?)\s*$", line)
    if m and m.group(2) not in ("Total", "root"):   # the enclosing timers, not passes
        r["pass:" + m.group(2)] = float(m.group(1))
def text(exe):
    out = subprocess.run(["size", "-A", exe], capture_output=True, text=True).stdout
    return sum(int(l.split()[1]) for l in out.splitlines() if l.startswith(".text"))
import os
r["text_bytes"] = text(f"{tmp}/wtc.exe")
if os.path.exists(f"{tmp}/generic.exe"):
    r["text_bytes_generic"] = text(f"{tmp}/generic.exe")
print(json.dumps(r))
EOF
  done
done

python3 - "$RUNS" > "$OUT/SUMMARY.md" <<'EOF'
import json, statistics, sys
from collections import defaultdict
runs = [json.loads(l) for l in open(sys.argv[1])]
by = defaultdict(list)
for r in runs:
    by[r["variant"]].append(r)
med = lambda rows, k: statistics.median(r[k] for r in rows if k in r) if any(k in r for r in rows) else None
print("# Compile cost\n")
print("Medians over the runs of each source; wall time in seconds. plain is the same clang compiling the "
      "source without ClangIR; wtc-cc is the sum of its four steps; the passes are wtc-opt's own timing. "
      "Not a reserved measurement: read the ratios, not the third digit.\n")
print("| source | n | plain | wtc-cc | ratio | CIR | wtc-opt | cir-translate | backend | .text | .text generic |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for v in sorted(by):
    rows = by[v]
    p, w = med(rows, "plain_s"), med(rows, "wtc_cc_s")
    cells = [f"{x:.3f}" if x is not None else "-" for x in (p, w)]
    cells.append(f"{w / p:.2f}×" if p and w else "-")
    cells += [f"{med(rows, k):.3f}" if med(rows, k) is not None else "-"
              for k in ("cir_s", "wtc_opt_s", "cir_translate_s", "backend_s")]
    cells += [str(int(med(rows, k))) if med(rows, k) is not None else "-" for k in ("text_bytes", "text_bytes_generic")]
    print(f"| {v} | {len(rows)} | " + " | ".join(cells) + " |")
passes = sorted({k for r in runs for k in r if k.startswith("pass:")})
if passes:
    print("\n## wtc-opt by pass, all sources\n")
    print("| pass | median s per source | share of wtc-opt |")
    print("|---|---|---|")
    total = statistics.median(r.get("wtc_opt_s", 0) for r in runs)
    for k in sorted(passes, key=lambda k: -statistics.median(r.get(k, 0) for r in runs)):
        m = statistics.median(r.get(k, 0) for r in runs)
        print(f"| {k[5:]} | {m:.4f} | {100 * m / total:.1f}% |" if total else f"| {k[5:]} | {m:.4f} | - |")
EOF
echo "summary: $OUT/SUMMARY.md"
