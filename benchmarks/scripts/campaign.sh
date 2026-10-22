#!/usr/bin/env bash
#
# Runs one measurement leg and writes benchmarks/results/<date>-<leg>/.
#
#   campaign.sh <leg> <build-dir> [reps]
#
# Legs: w1 (throughput), w2 (open-loop latency at 1 us and 10 us), w2pp
# (ping-pong), smoke (everything once, tiny sizes, no environment check);
# the MVP's own: w1mvp, w2mvp, w3wtc, w7, w8; the evaluation's: eval-w1,
# eval-w1smt, eval-w2, eval-w3, eval-w3grain, eval-w7, eval-w8, eval-w9,
# eval-ferret, eval-dedup, each with the question it answers in its comment
# below.
#
# Legs that run builds made by tools/wtc-cc take the directory holding them
# as BUILD and need the CMake build too, for the controls and the
# allocation counter: CONTROL_BUILD=<cmake build dir>.
#
# Run this INSIDE a `bench` reservation (see benchmarks/README.md) and never
# build inside one. Every measured run takes the host's exclusive measurement
# lock for its own duration, per the agreed rule that a leg locks per run.
#
# The allocator is a factor. Every queue that keeps its elements on the heap
# pays the allocator on every element, from one thread to another, and
# "use a better allocator" is the first objection to a result measured on
# glibc alone. The evaluation's legs therefore run each variant under
# several allocators, preloaded: glibc (none), jemalloc, tcmalloc, mimalloc
# and snmalloc, looked up by file name in WTC_ALLOC_DIR (default
# .apps/allocs at the root of the tree, outside version control; the
# README says how to fill it). WTC_ALLOCS="glibc jemalloc" narrows a leg
# to those; an allocator whose library is missing is skipped and logged.
# Each run records its allocator ("alloc"), so the summary keys on it. The
# controls, the warm-up and the explaining pass run on glibc.
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
CONTROL_BUILD=${CONTROL_BUILD:-$BUILD}
# Absolute, because a leg may run its programs from their input directory.
BUILD=$(cd "$BUILD" && pwd); CONTROL_BUILD=$(cd "$CONTROL_BUILD" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DATE=$(date -u +%Y-%m-%d)
OUT="$ROOT/benchmarks/results/$DATE-$LEG"
mkdir -p "$OUT"
RUNS="$OUT/runs.jsonl"
: > "$RUNS"

VARIANTS_SPSC="orig_generic shim_generic shim_spsc shim_spsc_inline ring_sp ring_borrowed ring_inline std_mutex moodycamel_rw_sp moodycamel_rw_inline moodycamel_cq_sp rigtorp_sp rigtorp_inline boost_spsc_sp boost_ms_queue"

WTC_ALLOC_DIR=${WTC_ALLOC_DIR:-$ROOT/.apps/allocs}
ALLOC_PRELOAD=""   # the library of the current allocator; empty for glibc

alloc_lib() { # name -> library file name, or nothing for glibc
  case "$1" in
    glibc)    echo "" ;;
    jemalloc) echo libjemalloc.so.2 ;;
    tcmalloc) echo libtcmalloc_minimal.so.4 ;;
    mimalloc) echo libmimalloc.so ;;
    snmalloc) echo libsnmalloc.so ;;
    *) echo "unknown allocator $1" >&2; return 2 ;;
  esac
}

# The allocators a leg will run: its default set, narrowed by WTC_ALLOCS,
# minus those whose library is not there (logged, not fatal: a host without
# snmalloc still measures everything else).
allocs_for() { # default list
  local want=${WTC_ALLOCS:-$*} out="" a lib
  for a in $want; do
    lib=$(alloc_lib "$a") || exit 2
    if [ -z "$lib" ] || [ -r "$WTC_ALLOC_DIR/$lib" ]; then out="$out $a"
    else echo "allocator $a skipped: $WTC_ALLOC_DIR/$lib not found" | tee -a "$OUT/log.txt" >&2; fi
  done
  echo "$out"
}

# Selects the allocator for the runs that follow: the name goes to every
# results line through WTC_ALLOC, the library to the run through LD_PRELOAD.
use_alloc() {
  local lib; lib=$(alloc_lib "$1") || exit 2
  export WTC_ALLOC=$1
  ALLOC_PRELOAD=${lib:+$WTC_ALLOC_DIR/$lib}
}

lock() {
  # Exclusive measurement lock for one run. --no-scope: inside a bench shell a
  # user scope may not be creatable, and the memory cap it would give is not
  # needed for a benchmark of a few hundred megabytes.
  if [ -n "$ALLOC_PRELOAD" ]; then set -- env "LD_PRELOAD=$ALLOC_PRELOAD" "$@"; fi
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
    # Dirty means the source differs from the commit; the results of the
    # legs before this one in a chain are output, not source.
    echo "  \"git_sha\": \"$(git -c safe.directory="$ROOT" -C "$ROOT" rev-parse HEAD)\", \"git_dirty\": $([ -z "$(git -c safe.directory="$ROOT" -C "$ROOT" status --porcelain -- . ':(exclude)benchmarks/results')" ] && echo false || echo true),"
    echo "  \"kernel\": \"$(uname -r)\","
    echo "  \"cpu_model\": \"$(lscpu | sed -n 's/^Model name: *//p')\","
    echo "  \"sockets\": \"$(lscpu | sed -n 's/^Socket(s): *//p')\", \"numa_nodes\": \"$(lscpu | sed -n 's/^NUMA node(s): *//p')\","
    echo "  \"allowed_cpus\": \"$(taskset -pc $$ | sed 's/.*: //')\","
    echo "  \"smt_control\": \"$(cat /sys/devices/system/cpu/smt/control 2>/dev/null || echo unknown)\","
    echo "  \"smt_siblings\": \"$("$HERE/check_env.sh" 2>/dev/null | sed -n 's/^ *smt siblings *//p')\","
    echo "  \"load_average\": \"$(cut -d' ' -f1-3 /proc/loadavg)\","
    echo "  \"build\": \"$(tr '\n' ';' < "$BUILD/build-info.txt" 2>/dev/null || echo 'built by wtc-cc')\","
    # What the numbers depend on beyond the source: the frequency policy the
    # environment check enforced, and the versions of everything compared.
    local cpu0
    cpu0=$(taskset -pc $$ | sed 's/.*: //' | cut -d, -f1 | cut -d- -f1)
    echo "  \"governor\": \"$(cat /sys/devices/system/cpu/cpu$cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)\","
    echo "  \"no_turbo\": \"$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo "boost=$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo unknown)")\","
    echo "  \"freq_khz\": \"$(cat /sys/devices/system/cpu/cpu$cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo unknown)\","
    echo "  \"llvm_sha\": \"$(git -c safe.directory="$ROOT" -C "$ROOT" submodule status llvm 2>/dev/null | awk '{print $1}' | tr -d '+-')\","
    echo "  \"boost\": \"$(sed -n 's/^#define BOOST_LIB_VERSION \"\(.*\)\"/\1/p' /usr/include/boost/version.hpp 2>/dev/null)\","
    echo "  \"tbb\": \"$(dpkg-query -W -f='${Version}' libtbb12 2>/dev/null || echo none)\","
    echo "  \"vendored\": \"$(sed -n 's/^| \([a-z]*\)\/ | [^|]* | \([0-9a-f]\{12\}\).*/\1@\2/p' "$ROOT/benchmarks/third_party/VENDORED.md" | tr '\n' ' ')\","
    echo "  \"control_build\": \"$CONTROL_BUILD\","
    echo "  \"alloc_dir\": \"$WTC_ALLOC_DIR\", \"allocators\": \"$(ls "$WTC_ALLOC_DIR" 2>/dev/null | grep '\.so' | tr '\n' ' ')\","
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
# A leg whose reservation is not two whole cores passes its placement, so
# that its controls run where its measurements do.
control() { # tag [placement arguments]
  local tag=$1; shift
  if [ ! -x "$CONTROL_BUILD/w1_throughput" ]; then
    echo "no controls: set CONTROL_BUILD to the CMake build" | tee -a "$OUT/log.txt"; return 0
  fi
  use_alloc glibc
  for k in 1 2 3; do
    lock "$CONTROL_BUILD/w1_throughput" --variant shim_generic --elem 64 --wait spin --rep "$tag" "$@" --json "$RUNS" 2>>"$OUT/log.txt"
  done
}

# Whether this build has the oneTBB baseline: a machine without oneTBB
# builds everything else, and a leg must not die on the one variant missing.
have_tbb() { grep -qiE '^tbb=(1|true|on)' "$CONTROL_BUILD/build-info.txt" 2>/dev/null; }

# A run that explains rather than times: allocator calls through the
# counting preload and user-space hardware counters, every line marked
# pass=mechanism so the summary keeps it out of the timing tables. Kept
# apart from the timed runs because counting perturbs what it counts.
mech() {
  use_alloc glibc   # the counting preload is an allocator of its own
  WTC_PASS=mechanism WTC_PERF=1 LD_PRELOAD="$CONTROL_BUILD/libwtc_alloccount.so" lock "$@"
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
warmup() { # [placement arguments]
  [ -x "$CONTROL_BUILD/w1_throughput" ] || return 0
  use_alloc glibc
  for k in 1 2; do
    lock "$CONTROL_BUILD/w1_throughput" --variant shim_generic --elem 64 --wait spin \
         --n 20000000 "$@" --json "" 2>>"$OUT/log.txt" >/dev/null
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
      for v in orig_generic shim_generic std_mutex moodycamel_cq_sp boost_ms_queue $(have_tbb && echo onetbb_cq_sp); do
        lock "$BUILD/w5_mpmc_control" --variant "$v" --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt"
      done
    done
    ;;
  eval-w1)
    # RQ2/RQ3: the queue alone, every baseline at three element sizes.
    # B0 shim_generic, orig_generic; B1 the general-purpose libraries; B2
    # the expert single-producer queues; the shim's own tags; the bare ring
    # moved, borrowed and inline. Then the explaining pass, three runs each
    # at a size that keeps it short. Needs a 2-core reservation.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    EVAL_W1="shim_generic orig_generic $(have_tbb && echo onetbb_cq_sp) moodycamel_cq_sp boost_ms_queue std_mutex rigtorp_sp rigtorp_inline moodycamel_rw_sp moodycamel_rw_inline boost_spsc_sp shim_spsc shim_spsc_inline ring_sp ring_borrowed ring_inline"
    ALLOCS=$(allocs_for glibc jemalloc tcmalloc mimalloc snmalloc)
    warmup; control 900
    for rep in $(seq 0 $((REPS-1))); do
      for alloc in $ALLOCS; do
        use_alloc "$alloc"
        for elem in 8 64 256; do
          for v in $EVAL_W1; do run_w1 "$rep" "$v" "$elem" spin; done
        done
      done
    done
    control 901
    for rep in 0 1 2; do
      for elem in 8 64 256; do
        for v in $EVAL_W1; do
          mech "$BUILD/w1_throughput" --variant "$v" --elem "$elem" --rep "$rep" --n 4000000 --json "$RUNS" 2>>"$OUT/log.txt"
        done
      done
    done
    ;;
  eval-w1smt)
    # Placement: the two threads on the SMT siblings of one core. Needs a
    # reservation that keeps the siblings online (bench without -e).
    "$HERE/check_env.sh" 2 || exit 1
    meta
    ALLOCS=$(allocs_for glibc jemalloc)
    warmup --smt-siblings; control 900 --smt-siblings
    for rep in $(seq 0 $((REPS-1))); do
      for alloc in $ALLOCS; do
        use_alloc "$alloc"
        for v in shim_generic $(have_tbb && echo onetbb_cq_sp) rigtorp_inline moodycamel_rw_sp shim_spsc shim_spsc_inline; do
          WTC_PLACEMENT=smt lock "$BUILD/w1_throughput" --variant "$v" --elem 64 --wait spin --rep "$rep" \
               --n "$(elems_for "$v")" --smt-siblings --json "$RUNS" 2>>"$OUT/log.txt"
        done
      done
    done
    control 901 --smt-siblings
    ;;
  eval-w2)
    # RQ2: latency against offered load, up to and past saturation, for the
    # shim's tags and one queue of each baseline class. Latency runs from
    # each element's due time; a saturated run is marked. Needs 2 cores.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    ALLOCS=$(allocs_for glibc jemalloc mimalloc)
    warmup; control 900
    for rep in $(seq 0 $((REPS-1))); do
      for alloc in $ALLOCS; do
        use_alloc "$alloc"
        for interval in 100 200 300 500 700 1000 2000 5000 10000; do
          for v in shim_generic $(have_tbb && echo onetbb_cq_sp) moodycamel_cq_sp rigtorp_inline moodycamel_rw_inline shim_spsc shim_spsc_inline ring_borrowed; do
            run_w2 "$rep" "$v" 64 "$interval"
          done
        done
      done
    done
    control 901
    ;;
  eval-w3)
    # RQ2/RQ3 on the pipeline: the adapter pipeline with every baseline
    # class, and the compiler's pipeline in its four builds (FLAVOURS, made
    # by build_wtc_flavours.sh), closed loop and at five offered rates.
    # Needs a 3-core reservation.
    "$HERE/check_env.sh" 3 || exit 1
    FLAVOURS=${FLAVOURS:?FLAVOURS=<dir of the W3 flavours>}
    meta
    ALLOCS=$(allocs_for glibc jemalloc)
    warmup; control 900
    for rep in $(seq 0 $((REPS-1))); do
      for alloc in $ALLOCS; do
        use_alloc "$alloc"
        for rate in 0 500000 1000000 2000000 3000000 4000000; do
          n=$([ "$rate" = 0 ] && echo 10000000 || echo 4000000)
          for v in shim_generic $(have_tbb && echo onetbb_cq_sp) moodycamel_cq_sp moodycamel_rw_sp moodycamel_rw_inline rigtorp_inline shim_spsc shim_spsc_inline; do
            lock "$BUILD/w3_pipeline" --variant "$v" --rate "$rate" --n "$n" --rep "$rep" --json "$RUNS" \
              2>>"$OUT/log.txt" >>"$OUT/checksums-$n.txt"
          done
          for f in generic hand hand-inline compiler; do
            lock "$FLAVOURS/w3_$f" --flavour "$f" --rate "$rate" --n "$n" --rep "$rep" --json "$RUNS" \
              2>>"$OUT/log.txt" | awk '{print $1, $2}' >>"$OUT/checksums-$n.txt"
          done
        done
      done
    done
    control 901
    for f in "$OUT"/checksums-*.txt; do
      echo "$(basename "$f"): $(sort -u "$f" | wc -l) distinct checksum(s) (must be 1)" | tee -a "$OUT/log.txt"
    done
    for f in generic compiler; do
      mech "$FLAVOURS/w3_$f" --flavour "$f" --n 4000000 --rep 0 --json "$RUNS" 2>>"$OUT/log.txt" >/dev/null
    done
    ;;
  eval-w9)
    # RQ2/RQ5: the general-purpose queues as threads are added -- whether the
    # shim's, which every unproved queue keeps, is a fair baseline beside the
    # libraries, and what shapes the compiler leaves to it (one producer into
    # many, many into many); with one consumer, the shim's list for many
    # producers as well, which the compiler substitutes for an mpsc verdict,
    # and with one producer its ring for many consumers (spmc).
    # Split producers and consumers, and the pairwise workload of the
    # lock-free queue literature. Needs a 16-core reservation.
    "$HERE/check_env.sh" 16 || exit 1
    meta
    MPMC="shim_generic orig_generic std_mutex moodycamel_cq_sp boost_ms_queue $(have_tbb && echo onetbb_cq_sp)"
    ALLOCS=$(allocs_for glibc jemalloc mimalloc)
    warmup; control 900
    for rep in $(seq 0 $((REPS-1))); do
      for alloc in $ALLOCS; do
        use_alloc "$alloc"
        for pc in 1:1 2:2 4:4 8:8 1:2 1:4 1:8 2:1 4:1 8:1; do
          # With one consumer, the shim's queue for many producers too, and
          # with one producer its ring for many consumers: what the compiler
          # substitutes for an mpsc or an spmc verdict.
          vs=$MPMC; [ "${pc##*:}" = 1 ] && vs="$vs shim_mpsc orig_mpsc"; [ "${pc%%:*}" = 1 ] && vs="$vs shim_spmc"
          for v in $vs; do
            lock "$BUILD/w9_mpmc_scaling" --variant "$v" --mode split --producers "${pc%%:*}" --consumers "${pc##*:}" \
              --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt"
          done
        done
        for t in 1 2 4 8 16; do
          for v in $MPMC; do
            lock "$BUILD/w9_mpmc_scaling" --variant "$v" --mode pairwise --threads "$t" --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt"
          done
        done
      done
    done
    control 901
    ;;
  eval-w7)
    # RQ4: batching a producer's pushes. Every flavour build_w7_flavours.sh
    # made (BUILD): as fast as it can, then paced at one element a
    # microsecond, where the delay an element waits for its batch shows.
    # Needs a 2-core reservation and CONTROL_BUILD.
    "$HERE/check_env.sh" 2 || exit 1
    meta
    warmup; control 900
    w7_n() { case "$1" in *generic*) echo 3000000 ;; *spsc*) echo 5000000 ;; *) echo 20000000 ;; esac; }
    ALLOCS=$(allocs_for glibc jemalloc)
    for rep in $(seq 0 $((REPS-1))); do
      for alloc in $ALLOCS; do
      use_alloc "$alloc"
      for b in "$BUILD"/w7_*; do
        [ -x "$b" ] || continue
        f=$(basename "$b"); f=${f#w7_}
        # The checksum depends on the element type and the count, which
        # differs by implementation: one file per pair, one checksum each.
        lock "$b" --flavour "$f" --n "$(w7_n "$f")" --rep "$rep" --json "$RUNS" \
          2>>"$OUT/log.txt" | awk '{print $1, $2}' >>"$OUT/checksums-${f##*_}-n$(w7_n "$f").txt"
        case "$f" in *_e64)
          lock "$b" --flavour "$f" --n 2000000 --rate 1000000 --rep "$rep" --json "$RUNS" 2>>"$OUT/log.txt" >/dev/null ;;
        esac
      done
      done
    done
    control 901
    for f in "$OUT"/checksums-*.txt; do
      echo "$(basename "$f"): $(sort -u "$f" | wc -l) distinct checksum(s) (must be 1)" | tee -a "$OUT/log.txt"
    done
    ;;
  eval-w8)
    # RQ4: batching a loop's critical sections, against the thread count and
    # the work between sections, where holding the mutex across that work
    # stops paying; then the batch size at two threads. Every flavour
    # build_w8_flavours.sh made (BUILD). Needs an 8-core reservation and
    # CONTROL_BUILD.
    "$HERE/check_env.sh" 8 || exit 1
    meta
    ALLOCS=$(allocs_for glibc jemalloc)
    warmup; control 900
    for rep in $(seq 0 $((REPS-1))); do
      for alloc in $ALLOCS; do
      use_alloc "$alloc"
      for t in 1 2 4 8; do
        for w in 0 20 50 100 200 500; do
          for f in plain batched hand; do
            lock "$BUILD/w8_$f" --flavour "$f" --threads "$t" --work-ns "$w" --n 2000000 --rep "$rep" --json "$RUNS" \
              2>>"$OUT/log.txt" >>"$OUT/checksums-t$t.txt"
          done
        done
      done
      for w in 0 20 50 100 200 500; do
        for f in batched_k8 hand_k8 batched_k512 hand_k512; do
          lock "$BUILD/w8_$f" --flavour "$f" --threads 2 --work-ns "$w" --n 2000000 --rep "$rep" --json "$RUNS" \
            2>>"$OUT/log.txt" >>"$OUT/checksums-t2.txt"
        done
      done
      done
    done
    control 901
    for f in "$OUT"/checksums-*.txt; do
      echo "$(basename "$f"): $(sort -u "$f" | wc -l) distinct checksum(s) (must be 1)" | tee -a "$OUT/log.txt"
    done
    ;;
  eval-w3grain)
    # RQ2: where the gain goes. The pipeline with each stage's own work swept
    # from none to about 250 us a record (--work rounds of about 15 ns), the
    # adapter pipeline with a queue of each class beside the compiler's
    # pipeline in its four builds (FLAVOURS). A real pipeline sits somewhere
    # on this axis: ferret, at about 10 ms a record, far to the right.
    # Needs a 3-core reservation.
    "$HERE/check_env.sh" 3 || exit 1
    FLAVOURS=${FLAVOURS:?FLAVOURS=<dir of the W3 flavours>}
    meta
    warmup; control 900
    grain_n() { case "$1" in 0|4) echo 10000000 ;; 16) echo 4000000 ;; 64) echo 2000000 ;; 256) echo 500000 ;;
                             1024) echo 150000 ;; 4096) echo 40000 ;; *) echo 10000 ;; esac; }
    ALLOCS=$(allocs_for glibc jemalloc)
    for rep in $(seq 0 $((REPS-1))); do
      for alloc in $ALLOCS; do
      use_alloc "$alloc"
      for work in 0 4 16 64 256 1024 4096 16384; do
        n=$(grain_n "$work")
        for v in shim_generic $(have_tbb && echo onetbb_cq_sp) moodycamel_rw_inline rigtorp_inline shim_spsc shim_spsc_inline; do
          lock "$BUILD/w3_pipeline" --variant "$v" --work "$work" --n "$n" --rep "$rep" --json "$RUNS" \
            2>>"$OUT/log.txt" >>"$OUT/checksums-w$work.txt"
        done
        for f in generic hand hand-inline compiler; do
          lock "$FLAVOURS/w3_$f" --flavour "$f" --work "$work" --n "$n" --rep "$rep" --json "$RUNS" \
            2>>"$OUT/log.txt" | awk '{print $1, $2}' >>"$OUT/checksums-w$work.txt"
        done
      done
      done
    done
    control 901
    for f in "$OUT"/checksums-*.txt; do
      echo "$(basename "$f"): $(sort -u "$f" | wc -l) distinct checksum(s) (must be 1)" | tee -a "$OUT/log.txt"
    done
    ;;
  eval-ferret)
    # RQ2/RQ5 on an application: PARSEC ferret, simlarge, as PARSEC builds it
    # and as the port builds it (BUILD: the bin directory of
    # benchmarks/apps/ferret/build.sh; INPUTS: the directory of its
    # fetch-inputs.sh). One thread per stage, where the compiler proves four
    # of the five queues single-producer, and 2 and 4 per middle stage, where
    # it proves none. The time is ferret's own, of its parallel region; every
    # run's output is checked against PARSEC's. Needs 18 cores.
    "$HERE/check_env.sh" 18 || exit 1
    INPUTS=${INPUTS:?INPUTS=<dir of benchmarks/apps/ferret/fetch-inputs.sh>}
    meta
    warmup; control 900
    cd "$INPUTS/simlarge"
    lock "$BUILD/ferret-orig" corel lsh queries 10 20 1 reference.txt >/dev/null 2>&1
    ferret_run() { # variant, threads per middle stage, rep
      local v=$1 n=$2 rep=$3 t
      t=$(lock "$BUILD/ferret-$v" corel lsh queries 10 20 "$n" out.txt 2>/dev/null | sed -n 's/^QUERY TIME: \([0-9.]*\) seconds/\1/p')
      local same=0
      if [ "$n" = 1 ]; then cmp -s out.txt reference.txt && same=1
      else cmp -s <(sort out.txt) <(sort reference.txt) && same=1; fi
      [ "$same" = 1 ] || echo "ferret-$v n=$n rep $rep: output differs from PARSEC's" | tee -a "$OUT/log.txt"
      printf '{"workload": "ferret", "variant": "%s", "threads": %d, "rep": %d, "time_s": %s, "queries": %d, "output_ok": %d, "alloc": "%s", "cmd": "%s"}\n' \
        "$v" "$n" "$rep" "${t:-0}" "$(wc -l < out.txt)" "$same" "$WTC_ALLOC" "ferret-$v corel lsh queries 10 20 $n" >> "$RUNS"
    }
    ALLOCS=$(allocs_for glibc jemalloc)
    for rep in $(seq 0 $((REPS-1))); do
      for alloc in $ALLOCS; do
        use_alloc "$alloc"
        for v in orig generic-one compiler-one hand-one; do ferret_run "$v" 1 "$rep"; done
        for n in 2 4; do
          for v in orig generic compiler; do ferret_run "$v" "$n" "$rep"; done
        done
      done
    done
    cd "$ROOT"
    control 901
    ;;
  eval-dedup)
    # RQ1/RQ2 on an application the analysis must refuse as it is written:
    # PARSEC dedup on its native input (BUILD: the bin directory of
    # benchmarks/apps/dedup/build.sh; INPUTS: the directory of its
    # fetch-inputs.sh). One thread per stage: the port as written (refused:
    # its queues are elements of arrays), with its queues as globals
    # (refused: the threads start in a function called from another
    # file), the same in one translation unit (three of four queues proved
    # single-producer), and tagged by hand; then 2 and 4 threads per middle
    # stage, where no queue is single-producer. The time is PARSEC's own
    # region of interest; the output goes to memory (SCRATCH, /dev/shm by
    # default), so the disk is not timed, and every run's is checked against
    # PARSEC's byte for byte. The mean chunk size, with the input's size,
    # gives the elements the queues carry. Needs 18 cores.
    "$HERE/check_env.sh" 18 || exit 1
    INPUTS=${INPUTS:?INPUTS=<dir of benchmarks/apps/dedup/fetch-inputs.sh>}
    ISO="$INPUTS/native/FC-6-x86_64-disc1.iso"
    SCRATCH=$(mktemp -d "${SCRATCH:-/dev/shm}/wtc-dedup.XXXXXX")
    meta
    warmup; control 900
    lock "$BUILD/dedup-orig" -c -p -t 1 -i "$ISO" -o "$SCRATCH/reference.ddp" >/dev/null 2>&1
    dedup_run() { # variant, threads per middle stage, rep
      local v=$1 n=$2 rep=$3 out t kb same=0
      out=$(lock "$BUILD/dedup-$v" -c -p -v -t "$n" -i "$ISO" -o "$SCRATCH/out.ddp" 2>&1)
      t=$(sed -n 's/.*Total time spent in ROI: \([0-9.]*\)s.*/\1/p' <<<"$out")
      kb=$(sed -n 's/^Mean data chunk size: *\([0-9.]*\) KB.*/\1/p' <<<"$out")
      cmp -s "$SCRATCH/out.ddp" "$SCRATCH/reference.ddp" && same=1
      [ "$same" = 1 ] || echo "dedup-$v n=$n rep $rep: output differs from PARSEC's" | tee -a "$OUT/log.txt"
      printf '{"workload": "dedup", "variant": "%s", "threads": %d, "rep": %d, "time_s": %s, "chunk_kb": %s, "input_bytes": %d, "output_ok": %d, "cmd": "%s"}\n' \
        "$v" "$n" "$rep" "${t:-0}" "${kb:-0}" "$(stat -c %s "$ISO")" "$same" "dedup-$v -c -p -v -t $n" >> "$RUNS"
      rm -f "$SCRATCH/out.ddp"
    }
    for rep in $(seq 0 $((REPS-1))); do
      for v in orig generic-one compiler-one hand-one hand-spsc-one compiler-flat-one compiler-flat-one-unity; do
        dedup_run "$v" 1 "$rep"
      done
      for n in 2 4; do
        for v in orig generic compiler; do dedup_run "$v" "$n" "$rep"; done
      done
    done
    rm -rf "$SCRATCH"
    control 901
    ;;
  *) echo "unknown leg $LEG" >&2; exit 2 ;;
esac

if ! python3 "$HERE/summarise.py" "$OUT" > "$OUT/SUMMARY.md"; then
  echo "summarise failed; the runs are in $OUT/runs.jsonl and can be summarised again" >&2
fi
BUILD=$BUILD CONTROL_BUILD=$CONTROL_BUILD FLAVOURS=${FLAVOURS:-} INPUTS=${INPUTS:-} \
  "$HERE/scrub_paths.sh" "$OUT"
echo "results: $OUT"
