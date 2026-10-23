#!/usr/bin/env bash
#
# Runs one measurement leg and writes benchmarks/results/<date>-<leg>/.
#
#   campaign.sh <leg> <build-dir> [reps]
#
# Legs: w1 (throughput), w2 (open-loop latency at 1 us and 10 us), w2pp
# (ping-pong), smoke (everything once, tiny sizes, no environment check);
# the MVP's own: w1mvp, w2mvp, w3wtc, w7, w8.
#
# Run this INSIDE a `bench` reservation (see benchmarks/README.md) and never
# build inside one. Every measured run takes the host's exclusive measurement
# lock for its own duration, per the agreed rule that a leg locks per run.
#
# Discipline encoded here rather than remembered:
#   - variants interleave round-robin across repetitions, so drift hits all
#     of them equally instead of the last one;
#   - each run is a fresh process;
#   - three control runs (shim_generic, E64) open and close the campaign, and
#     the summary compares their medians and, more sensitively, looks for a
#     trend across repetitions over every configuration at once;
#   - the environment is checked first and its state written to meta.json.
set -euo pipefail

LEG=${1:?leg}; BUILD=${2:?build dir}; REPS=${3:-10}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DATE=$(date -u +%Y-%m-%d)
OUT="$ROOT/benchmarks/results/$DATE-$LEG"
mkdir -p "$OUT"
RUNS="$OUT/runs.jsonl"
: > "$RUNS"

VARIANTS_SPSC="orig_generic shim_generic shim_spsc shim_spsc_inline ring_sp ring_borrowed ring_inline std_mutex moodycamel_rw_sp moodycamel_rw_inline moodycamel_cq_sp rigtorp_sp rigtorp_inline boost_spsc_sp boost_ms_queue"

lock() {
  # Exclusive measurement lock for one run. --no-scope: inside a bench shell a
  # user scope may not be creatable, and the memory cap it would give is not
  # needed for a benchmark of a few hundred megabytes.
  if command -v machine-lock >/dev/null; then
    machine-lock --lane wtc-bench --measure --minutes 5 --why "wtc $LEG" --no-scope -- "$@"
  else
    "$@"
  fi
}

meta() {
  {
    echo "{"
    echo "  \"leg\": \"$LEG\", \"date\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
    echo "  \"git_sha\": \"$(git -C "$ROOT" rev-parse HEAD)\", \"git_dirty\": $([ -z "$(git -C "$ROOT" status --porcelain)" ] && echo false || echo true),"
    echo "  \"kernel\": \"$(uname -r)\","
    echo "  \"cpu_model\": \"$(lscpu | sed -n 's/^Model name: *//p')\","
    echo "  \"sockets\": \"$(lscpu | sed -n 's/^Socket(s): *//p')\", \"numa_nodes\": \"$(lscpu | sed -n 's/^NUMA node(s): *//p')\","
    echo "  \"allowed_cpus\": \"$(taskset -pc $$ | sed 's/.*: //')\","
    echo "  \"smt_control\": \"$(cat /sys/devices/system/cpu/smt/control 2>/dev/null || echo unknown)\","
    echo "  \"smt_siblings\": \"$("$HERE/check_env.sh" 2>/dev/null | sed -n 's/^ *smt siblings *//p')\","
    echo "  \"load_average\": \"$(cut -d' ' -f1-3 /proc/loadavg)\","
    echo "  \"build\": \"$(tr '\n' ';' < "$BUILD/build-info.txt" 2>/dev/null || echo 'built by wtc-cc')\","
    echo "  \"reps\": $REPS"
    echo "}"
  } > "$OUT/meta.json"
}

run_w1() {
  local rep=$1 v=$2 elem=$3 wait=$4
  lock "$BUILD/w1_throughput" --variant "$v" --elem "$elem" --wait "$wait" --rep "$rep" \
       --n "$(elems_for "$v")" --json "$RUNS" 2>>"$OUT/log.txt"
}
run_w2() {
  local rep=$1 v=$2 elem=$3 interval=$4 extra=${5:-}
  lock "$BUILD/w2_latency" --variant "$v" --elem "$elem" --interval-ns "$interval" --rep "$rep" --json "$RUNS" $extra 2>>"$OUT/log.txt"
}

# Three runs, not one. The control compares the start of a leg with its end,
# and shim_generic's own run-to-run spread is around 8%, so comparing two
# single runs against a tight threshold flags every leg and means nothing.
control() { # tag
  for k in 1 2 3; do
    lock "$BUILD/w1_throughput" --variant shim_generic --elem 64 --wait spin --rep "$1" --json "$RUNS" 2>>"$OUT/log.txt"
  done
}

# Elements per run, by variant. A fixed count makes a run last 17 seconds for
# the slowest queue and 0.1 seconds for the fastest, and a tenth of a second
# measures process startup rather than the queue: the two inline rings came
# out with a 15% coefficient of variation for that reason alone. These counts
# put every run in the same few seconds.
elems_for() {
  case "$1" in
    rigtorp_inline)       echo 400000000 ;;
    ring_inline)          echo 200000000 ;;
    ring_borrowed)        echo 200000000 ;;
    shim_spsc_inline)     echo 100000000 ;;
    moodycamel_rw_inline) echo  40000000 ;;
    *)                    echo  20000000 ;;
  esac
}

# The first runs inside a fresh reservation are slow by themselves. One
# short discarded run was not enough: across three legs the opening controls
# still came out about 10% under the closing ones while the per-repetition
# trend stayed flat, which is the signature of a warm-up that ended too
# early rather than of drift. Two full-length runs, discarded.
warmup() {
  for k in 1 2; do
    lock "$BUILD/w1_throughput" --variant shim_generic --elem 64 --wait spin \
         --n 20000000 --json "" 2>>"$OUT/log.txt" >/dev/null
  done
}

case "$LEG" in
  smoke)
    meta
    for v in $VARIANTS_SPSC; do
      "$BUILD/w1_throughput" --variant "$v" --elem 64 --n 1000000 --rep 0 --json "$RUNS" 2>>"$OUT/log.txt"
    done
    for v in shim_generic shim_spsc ring_inline; do
      "$BUILD/w2_latency" --variant "$v" --elem 64 --samples 100000 --warm 10000 --rep 0 --json "$RUNS" 2>>"$OUT/log.txt"
    done
    ;;
  w1)
    "$HERE/check_env.sh" || exit 1
    meta
    warmup
    control 900
    for rep in $(seq 0 $((REPS-1))); do
      for elem in 8 64 256; do
        for v in $VARIANTS_SPSC; do run_w1 "$rep" "$v" "$elem" spin; done
      done
    done
    for rep in $(seq 0 2); do
      for v in orig_generic shim_generic shim_spsc ring_inline; do run_w1 "$rep" "$v" 64 yield; done
    done
    control 901
    ;;
  w2)
    "$HERE/check_env.sh" || exit 1
    meta
    warmup
    control 900
    for rep in $(seq 0 $((REPS-1))); do
      for interval in 1000 10000; do
        for v in orig_generic shim_generic shim_spsc ring_sp ring_inline std_mutex moodycamel_rw_sp moodycamel_cq_sp rigtorp_sp boost_spsc_sp; do
          run_w2 "$rep" "$v" 64 "$interval"
        done
      done
    done
    control 901
    ;;
  w2pp)
    "$HERE/check_env.sh" || exit 1
    meta
    for rep in $(seq 0 $((REPS-1))); do
      for v in shim_generic shim_spsc ring_inline moodycamel_rw_sp; do run_w2 "$rep" "$v" 64 0 --pingpong; done
    done
    ;;
  w3)
    # needs a 3-core reservation
    "$HERE/check_env.sh" 3 || exit 1
    meta
    for rep in $(seq 0 $((REPS-1))); do
      for v in orig_generic shim_generic shim_spsc ring_sp ring_inline moodycamel_rw_sp rigtorp_sp; do
        lock "$BUILD/w3_pipeline" --variant "$v" --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt" >>"$OUT/checksums.txt"
        lock "$BUILD/w3_pipeline" --variant "$v" --rate 2000000 --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt" >>"$OUT/checksums.txt"
      done
    done
    sort -u "$OUT/checksums.txt" | wc -l | xargs -I{} echo "distinct checksums across all W3 runs: {} (must be 1)" | tee -a "$OUT/log.txt"
    ;;
  w3wtc)
    # The compiler-selected comparison. Needs a 3-core reservation and the
    # flavours already built by build_wtc_flavours.sh; BUILD is the
    # directory holding them.
    "$HERE/check_env.sh" 3 || exit 1
    meta
    for rep in $(seq 0 $((REPS-1))); do
      for f in generic hand hand-inline compiler; do
        lock "$BUILD/w3_$f" --flavour "$f" --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt" >>"$OUT/checksums.txt"
        lock "$BUILD/w3_$f" --flavour "$f" --rate 2000000 --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt" >>"$OUT/checksums.txt"
      done
    done
    # Each line also names the implementation the run carried, which
    # differs between builds by design: compare the checksums alone.
    echo "distinct checksums across all runs: $(awk '{print $2}' "$OUT/checksums.txt" | sort -u | wc -l) (must be 1)" | tee -a "$OUT/log.txt"
    ;;
  w1mvp)
    # The queue numbers the MVP quotes, and only those: the general-purpose
    # shim, its spsc and spsc_inline tags, and the bare ring popped by moving
    # and by borrowing, at 8 and 64 bytes. Needs a 2-core reservation.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    warmup
    for rep in $(seq 0 $((REPS-1))); do
      for elem in 8 64; do
        for v in shim_generic shim_spsc shim_spsc_inline ring_borrowed ring_inline; do
          run_w1 "$rep" "$v" "$elem" spin
        done
      done
    done
    ;;
  w7)
    # Batching a producer's pushes: twelve flavours built by
    # build_w7_flavours.sh into BUILD (three implementations, batched or
    # not, two element sizes). Needs a 2-core reservation.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    for rep in $(seq 0 $((REPS-1))); do
      for e in e8 e64; do
        for f in generic_plain generic_batch spsc_plain spsc_batch inline_plain inline_batch; do
          lock "$BUILD/w7_${f}_$e" --flavour "$f" --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt" >>"$OUT/checksums.txt"
        done
      done
    done
    # Each line also names the implementation the run carried, which
    # differs between builds by design: compare the checksums alone.
    echo "distinct checksums across all runs: $(awk '{print $2}' "$OUT/checksums.txt" | sort -u | wc -l) (must be 1)" | tee -a "$OUT/log.txt"
    ;;
  smt)
    # The two threads on one physical core, sharing its caches. A spinning
    # consumer steals the issue slots its sibling needs, which is the leg
    # that says whether the wait strategy is worth a transformation.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    warmup; control 900
    for rep in $(seq 0 $((REPS-1))); do
      for v in orig_generic shim_generic shim_spsc ring_sp ring_inline; do
        lock "$BUILD/w1_throughput" --variant "$v" --elem 64 --wait spin --rep "$rep" \
             --n "$(elems_for "$v")" --smt-siblings --json "$RUNS" 2>>"$OUT/log.txt"
      done
      for v in shim_generic shim_spsc ring_inline; do
        lock "$BUILD/w1_throughput" --variant "$v" --elem 64 --wait yield --rep "$rep" \
             --n "$(elems_for "$v")" --smt-siblings --json "$RUNS" 2>>"$OUT/log.txt"
      done
    done
    control 901
    ;;
  far)
    # The lowest and highest cores of the reservation rather than the two
    # lowest. On a 56-core die the distance between tiles is not exposed, so
    # this is informative only: it says whether placement matters at all.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    LO=$(taskset -pc $$ | sed 's/.*: //' | tr ',' '\n' | head -1 | cut -d- -f1)
    HI=$(taskset -pc $$ | sed 's/.*: //' | tr ',' '\n' | tail -1 | cut -d- -f2)
    warmup; control 900
    for rep in $(seq 0 $((REPS-1))); do
      for v in orig_generic shim_generic shim_spsc ring_inline; do
        lock "$BUILD/w1_throughput" --variant "$v" --elem 64 --wait spin --rep "$rep" \
             --n "$(elems_for "$v")" --cpus "$LO,$HI" --json "$RUNS" 2>>"$OUT/log.txt"
      done
    done
    control 901
    ;;
  tbb)
    # oneTBB against the other general-purpose queues, and against the
    # specialised one for scale. Measured in its own leg rather than added
    # to an old table, because rows compared across sessions are rows
    # nothing controlled.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    warmup; control 900
    for rep in $(seq 0 $((REPS-1))); do
      for elem in 8 64 256; do
        for v in onetbb_cq_sp moodycamel_cq_sp boost_ms_queue std_mutex shim_generic shim_spsc; do
          run_w1 "$rep" "$v" "$elem" spin
        done
      done
    done
    control 901
    ;;
  w1check)
    # The same throughput rows from a build with the runtime contract check
    # compiled in, so the check's cost can be read against the w1 leg. The
    # check is what turns every static verdict into a claim the program
    # tests at run time, and a cost nobody has measured is a cost nobody
    # will pay.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    warmup; control 900
    for rep in $(seq 0 $((REPS-1))); do
      for elem in 8 64; do
        for v in shim_spsc ring_sp ring_inline shim_generic; do run_w1 "$rep" "$v" "$elem" spin; done
      done
    done
    control 901
    ;;
  w6)
    # The ring's own shape: block size and cache-line padding.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    # One process per configuration per repetition, and the configurations
    # interleaved inside each repetition, so a configuration is never
    # measured only at the start or only at the end.
    for rep in $(seq 0 $((REPS-1))); do
      for cfg in 0 1 2 3 4 5 6 7 8; do
        lock "$BUILD/w6_ring_shape" --config "$cfg" --n 60000000 --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt"
      done
    done
    ;;
  w2mvp)
    # Open-loop latency for the implementations the MVP substitutes: the
    # general-purpose shim, its spsc and spsc_inline tags, and the bare ring
    # popped by borrowing, at 1 us and 10 us. Needs a 2-core reservation.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    warmup
    for rep in $(seq 0 $((REPS-1))); do
      for interval in 1000 10000; do
        for v in shim_generic shim_spsc shim_spsc_inline ring_borrowed; do
          run_w2 "$rep" "$v" 64 "$interval"
        done
      done
    done
    ;;
  w8)
    # Critical sections held as one: the two builds by build_w8_flavours.sh
    # into BUILD, alone and with two threads contending, with no work of
    # their own between sections and with some. Needs a 2-core reservation.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    for rep in $(seq 0 $((REPS-1))); do
      for t in 1 2; do
        for w in 0 64; do
          for f in plain batched; do
            lock "$BUILD/w8_$f" --flavour "$f" --threads "$t" --work "$w" --rep "$rep" --json "$RUNS" \
              2>>"$OUT/log.txt" >>"$OUT/checksums.txt"
          done
        done
      done
    done
    echo "distinct checksums per thread count: $(sort -u "$OUT/checksums.txt" | wc -l) (must be 2, one per count)" | tee -a "$OUT/log.txt"
    ;;
  w5)
    # needs a 4-core reservation
    "$HERE/check_env.sh" 4 || exit 1
    meta
    for rep in $(seq 0 $((REPS-1))); do
      for v in orig_generic shim_generic std_mutex moodycamel_cq_sp boost_ms_queue onetbb_cq_sp; do
        lock "$BUILD/w5_mpmc_control" --variant "$v" --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt"
      done
    done
    ;;
  *) echo "unknown leg $LEG" >&2; exit 2 ;;
esac

if ! python3 "$HERE/summarise.py" "$OUT" > "$OUT/SUMMARY.md"; then
  echo "summarise failed; the runs are in $OUT/runs.jsonl and can be summarised again" >&2
fi
echo "results: $OUT"
