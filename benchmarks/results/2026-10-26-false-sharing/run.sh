#!/usr/bin/env bash
# False sharing where it really exists: pre-fix / pre-fix + align pass /
# hand fix (W7), pre-fix / hand fix (W3, W2: stack locals, out of the align
# pass's reach), Phoenix linear_regression unpadded / padded at -O2 and -O0.
# Untimed host: paired, shuffled rounds; every run records its effective
# frequency (cycles:u / task-clock) and how busy the processors outside its
# set were. All binaries are static.
#
#   ./run.sh <out-dir> [rounds=20]          then: python3 -I analyse.py <out-dir>
#   C2C=1 ./run.sh <out-dir> 0              only try perf c2c on two known cases
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?out dir}; ROUNDS=${2:-20}
B=$HERE/bin
# Processors: two (or three) neighbouring cores for the queue workloads, a
# set of LRN for linear_regression. Defaults are the second host's socket 1.
P0=${P0:-16} P1=${P1:-17} P2=${P2:-18} LRSET=${LRSET:-16-31} LRN=${LRN:-16}
mkdir -p "$OUT/runs"
DAT=$OUT/lr.dat
[ -s "$DAT" ] || head -c "${DATB:-200000000}" /dev/urandom > "$DAT"   # 100 M points

busy() { # busy and total jiffies over processors NOT in the comma list $1
  awk -v set=",$1," '/^cpu[0-9]/{c=substr($1,4); if (index(set, ","c",")) next;
    t=0; for(i=2;i<=NF;i++) t+=$i; b+=t-$5-$6; tt+=t} END{print b, tt}' /proc/stat
}

# Where perf may not be used (perf_event_paranoid 3) runs go without it and
# the frequency column stays empty.
PERF=(perf stat -x, -e cycles:u,task-clock -o)
perf stat -e task-clock true >/dev/null 2>&1 || { PERF=(); echo "perf stat unavailable: no frequency column"; }

run() { # job cpus cmd...
  local job=$1 cpus=$2; shift 2
  local f="$OUT/runs/r${r}_$job"
  # @JSON@ stands for this run's own results file: a program that opens
  # /dev/stdout writes over whatever it printed there before.
  set -- "${@//@JSON@/$f.json}"
  read -r b0 t0 < <(busy "$cpus")
  if [ ${#PERF[@]} -gt 0 ]; then "${PERF[@]}" "$f.perf" -- "$@" >"$f.out" 2>"$f.err"
  else : > "$f.perf"; "$@" >"$f.out" 2>"$f.err"; fi
  local rc=$?
  read -r b1 t1 < <(busy "$cpus")
  echo "$r $job $rc $((b1 - b0)) $((t1 - t0))" >> "$OUT/index.txt"
}

if [ "${C2C:-0}" = 1 ]; then
  for job in w7_pre_inline_e64 lr_orig_O0; do
    case $job in
      w7_*) cmd=(taskset -c $P0,$P1 "$B/$job" --cpus $P0,$P1) ;;
      lr_*) cmd=(env LR_THREADS=$LRN taskset -c $LRSET "$B/$job" "$DAT") ;;
    esac
    perf c2c record -o "$OUT/c2c-$job.data" -- "${cmd[@]}" >"$OUT/c2c-$job.log" 2>&1 \
      && perf c2c report -i "$OUT/c2c-$job.data" --stdio >>"$OUT/c2c-$job.log" 2>&1
    echo "c2c $job rc=$? (see $OUT/c2c-$job.log)"
  done
fi

JOBS=()
for impl in inline generic; do for e in e8 e64; do for v in pre prealign fix; do
  JOBS+=("w7_${v}_${impl}_$e"); done; done; done
for t in pre main; do for v in rigtorp_inline shim_spsc_inline shim_generic; do JOBS+=("w3_${t}:$v"); done; done
for t in pre main; do for v in shim_spsc_inline rigtorp_inline; do JOBS+=("w2_${t}:$v"); done; done
for o in O2 O0; do for v in orig pad; do JOBS+=("lr_${v}_$o"); done; done

for ((r = 0; r < ROUNDS; r++)); do
  for job in $(printf '%s\n' "${JOBS[@]}" | shuf); do
    case $job in
      w7_*) run "$job" $P0,$P1 "$B/$job" --cpus $P0,$P1 --json @JSON@ ;;
      w3_*) bin=${job%%:*}; v=${job#*:}
            run "${bin}_$v" $P0,$P1,$P2 "$B/$bin" --variant "$v" --cpus $P0,$P1,$P2 --json @JSON@ ;;
      w2_*) bin=${job%%:*}; v=${job#*:}
            run "${bin}_$v" $P0,$P1 "$B/$bin" --variant "$v" --cpus $P0,$P1 --interval-ns 1000 --json @JSON@ ;;
      lr_*) run "$job" "$(seq -s, ${LRSET%-*} ${LRSET#*-})" env LR_THREADS=$LRN taskset -c $LRSET "$B/$job" "$DAT" ;;
    esac
  done
  echo "round $r done"
done
echo DONE >> "$OUT/index.txt"
