#!/usr/bin/env bash
#
# What the compiler's choice adds to a lifted program, four ways per
# configuration: the program as released (orig), its queue lifted onto
# the general-purpose queue (generic, wtc-cc --no-specialize), lifted with
# the compiler's choice (compiler), and the hand port onto wtc::queue
# (port). Untimed: it pins nothing and fixes no frequency, and runs where
# it is started; the README of each application says what a timed leg
# would hold fixed. g3log is not here: built through ClangIR, lifted or
# not, it fails at run time (benchmarks/apps/g3log/README.md).
#
#   lift_attribution.sh <out-dir> <reps> <spdlog-bin> <ctpl-bin> <ferret-bin> <ferret-inputs> [<dedup-bin> <dedup-inputs>]
#
# The *-bin directories are the bin/ of each application's build.sh, and
# a program whose bin is given as "-" is left out; <ferret-inputs> holds
# the simlarge input and <dedup-inputs> the native one (each program's
# fetch-inputs.sh). Writes
# <out-dir>/raw.csv, one line per run, and <out-dir>/TABLE.md, the median
# of each configuration and the compiler/generic ratio. The variants of
# one configuration run in turn, rep by rep, so that a drift in the host
# touches each of them alike.
set -euo pipefail
OUT=${1:?out dir}; R=${2:?reps}
SPD=${3:?spdlog bin}; CTPL=${4:?ctpl bin}; FER=${5:?ferret bin}; FIN=${6:?ferret inputs}
DED=${7:--}; DIN=${8:-}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
RAW=$OUT/raw.csv
echo "program,config,variant,rep,value,unit" > "$RAW"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
row() { echo "$1,$2,$3,$4,$5,$6" >> "$RAW"; }

# spdlog: logging threads, 64-byte messages, a null sink, one worker;
# messages per second, from the first call until the worker has written
# the last.
[ "$SPD" = - ] || for t in 1 2 4 8; do
  for r in $(seq "$R"); do
    for v in orig:spdlog-orig generic:spdlog-lifted-generic compiler:spdlog-lifted port:spdlog-compiler; do
      x=$("$SPD/${v#*:}" --threads "$t" --n 500000 --payload 64 --sink null 2>&1 \
          | grep -oE '[0-9.]+ Mmsg/s' | grep -oE '^[0-9.]+')
      row spdlog "threads=$t" "${v%%:*}" "$r" "$x" Mmsg/s
    done
  done
done

# CTPL: one submitter, the pool's workers, tasks of the given work; tasks
# per second over one second.
[ "$CTPL" = - ] || for w in 1 2 4 8; do
  for work in 0 1000 10000; do
    for r in $(seq "$R"); do
      for v in orig:ctpl-orig generic:ctpl-generic compiler:ctpl-compiler port:ctpl-port; do
        x=$("$CTPL/${v#*:}" "$w" 1 "$work" 2>&1 | grep -oE 'Mtasks/s=[0-9.]+' | grep -oE '[0-9.]+$')
        row ctpl "workers=$w work_ns=$work" "${v%%:*}" "$r" "$x" Mtasks/s
      done
    done
  done
done

# ferret: simlarge, threads per middle stage; wall seconds, the output
# checked against the original's (bytes at one thread a stage, lines at
# more, whose order is the threads').
[ "$FER" = - ] || { cp -r "$FIN" "$TMP/fin"; cd "$TMP/fin"; }
[ "$FER" = - ] || for n in 1 4; do
  for r in $(seq "$R"); do
    for v in orig:ferret-orig generic:ferret-lifted-generic compiler:ferret-lifted port:ferret-compiler; do
      s=$(date +%s.%N)
      "$FER/${v#*:}" corel lsh queries 10 20 "$n" "out-${v%%:*}-$n.txt" >/dev/null 2>&1
      e=$(date +%s.%N)
      if [ "$n" = 1 ]; then cmp -s "out-${v%%:*}-$n.txt" "out-orig-$n.txt" || echo "ferret ${v%%:*} n=$n: output differs" >&2
      else cmp -s <(sort "out-${v%%:*}-$n.txt") <(sort "out-orig-$n.txt") || echo "ferret ${v%%:*} n=$n: output differs" >&2; fi
      row ferret "threads=$n" "${v%%:*}" "$r" "$(echo "$e - $s" | bc)" s
    done
  done
done

# dedup: the native input, threads per middle stage; seconds in the
# region of interest, from PARSEC's hooks, the archive checked against the
# original's byte for byte. The port is the one built as one unit with
# main (dedup-compiler-unity), whose analysis sees what the lifted
# build's sees.
ISO="$DIN/native/FC-6-x86_64-disc1.iso"
[ "$DED" = - ] || for t in 1 4; do
  "$DED/dedup-orig" -c -p -t "$t" -i "$ISO" -o "$TMP/ref.ddp" >/dev/null 2>&1
  for r in $(seq "$R"); do
    for v in orig:dedup-orig generic:dedup-lifted-generic compiler:dedup-lifted port:dedup-compiler-unity; do
      x=$("$DED/${v#*:}" -c -p -t "$t" -i "$ISO" -o "$TMP/out.ddp" 2>&1 | sed -n 's/.*ROI: \([0-9.]*\)s.*/\1/p' | head -1)
      cmp -s "$TMP/out.ddp" "$TMP/ref.ddp" || echo "dedup ${v%%:*} t=$t: output differs" >&2
      row dedup "threads=$t" "${v%%:*}" "$r" "$x" s
    done
  done
done

python3 "$HERE/lift_attribution.py" "$RAW" > "$OUT/TABLE.md"
cat "$OUT/TABLE.md"
