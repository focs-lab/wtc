#!/usr/bin/env bash
#
# One run of an unpinned leg (campaign.sh, WTC_UNPINNED=1): runs the command
# under `perf stat` and appends one row to $OUT/unpinned.tsv saying what
# the run cost and what happened around it.
#
#   unpinned_run.sh <command> [args...]
#
# A script, not a shell function, so that campaign.sh can hand it to
# machine-lock (or any other wrapper that runs its command as a program). It reads from
# the environment, which campaign.sh exports:
#   RUNS          the leg's runs.jsonl
#   OUT           the leg's result directory
#   WTC_LEG_CPUS  the leg's processor list, as taskset prints it
#   WTC_ALLOC     the allocator the run is preloaded with
#
# Columns of unpinned.tsv (a header line comes first):
#   run           the run's number within the leg, from 1
#   first, last   the lines of runs.jsonl the run wrote, or "-" for both
#                 when it wrote none: a warm-up, a run whose results line
#                 campaign.sh writes after it (ferret and dedup, their
#                 compiler builds and their PARSEC reference builds alike),
#                 or a run that failed. For ferret and dedup, take the rows
#                 with a results line in campaign.sh's order, by run number;
#                 the cmd column says which build each row is.
#   user_ghz      user-mode cycles over task-clock (nan when perf could
#                 not count either). task-clock includes
#                 time in the kernel and cycles:u does not, so a run that
#                 sleeps or yields in the kernel reads as slower-clocked
#                 than it ran; compare it between runs of one variant.
#   ref_ratio     user-mode cycles over user-mode reference cycles: the
#                 core clock as a multiple of the reference clock, where
#                 the processor counts reference cycles ("nan" where not)
#   cycles_u, task_clock_ms
#                 the two counts user_ghz is made of
#   outside_busy  the busy share of the processors outside WTC_LEG_CPUS
#                 over the run, from /proc/stat
#   alloc, cmd    the allocator, and the command line quoted as the shell
#                 would (printf %q), tabs replaced by spaces
#
# Needs perf, and perf_event_paranoid at most 2 for user-mode counts.
set -uo pipefail
: "${RUNS:?}" "${OUT:?}" "${WTC_LEG_CPUS:?}"
TSV="$OUT/unpinned.tsv"

expand() { # 0-3,8 -> ,0,1,2,3,8,
  local r out="," i
  IFS=, read -ra parts <<<"$1"
  for r in "${parts[@]}"; do
    if [[ $r == *-* ]]; then for ((i = ${r%-*}; i <= ${r#*-}; i++)); do out+="$i,"; done
    else out+="$r,"; fi
  done
  echo "$out"
}
SET=$(expand "$WTC_LEG_CPUS")

outside_busy() { # prints busy and total jiffies over the processors outside SET
  awk -v set="$SET" '/^cpu[0-9]/{c=substr($1,4); if (index(set, ","c",")) next;
    t=0; for(i=2;i<=NF;i++) t+=$i; b+=t-$5-$6; tt+=t} END{print b+0, tt+0}' /proc/stat
}

[ -s "$TSV" ] || printf 'run\tfirst\tlast\tuser_ghz\tref_ratio\tcycles_u\ttask_clock_ms\toutside_busy\talloc\tcmd\n' > "$TSV"
run=$(( $(wc -l < "$TSV") ))   # the header is line 1, so this is the next run's number
before=$(wc -l < "$RUNS")
read -r b0 t0 < <(outside_busy)
pf=$(mktemp)
# The workload runs under a shell that turns a death by signal into the
# status 128 + signal: perf stat itself exits 0 when its child is killed,
# and a leg must stop on a crashed run as a pinned one does.
perf stat -x, -e cycles:u,ref-cycles:u,task-clock -o "$pf" -- bash -c '"$@"; exit $?' _ "$@"
rc=$?
read -r b1 t1 < <(outside_busy)
after=$(wc -l < "$RUNS")
# perf -x, rows are value,unit,event,...; a counter the processor lacks
# reads "<not supported>" (reference cycles on AMD), one perf could not
# schedule "<not counted>": anything but a number becomes nan.
IFS=$'\t' read -r cyc ref tclk < <(awk -F, '
  function num(v) { return (v ~ /^[0-9]+(\.[0-9]+)?$/) ? v : "nan" }
  $3 ~ /^cycles/     {c=num($1)}
  $3 ~ /^ref-cycles/ {r=num($1)}
  $3 ~ /^task-clock/ {t=num($1)}
  END {printf "%s\t%s\t%s\n", (c==""?"nan":c), (r==""?"nan":r), (t==""?"nan":t)}' "$pf")
rm -f "$pf"
ghz=nan; ratio=nan
[ "$cyc" != nan ] && [ "$tclk" != nan ] && ghz=$(awk -v c="$cyc" -v t="$tclk" 'BEGIN{if (t>0) printf "%.3f", c/(t*1e6); else print "nan"}')
[ "$cyc" != nan ] && [ "$ref" != nan ] && ratio=$(awk -v c="$cyc" -v r="$ref" 'BEGIN{if (r>0) printf "%.3f", c/r; else print "nan"}')
busy=$(awk -v b=$((b1-b0)) -v t=$((t1-t0)) 'BEGIN{if (t>0) printf "%.3f", b/t; else print "nan"}')
if (( after > before )); then first=$((before+1)); last=$after; else first=-; last=-; fi
# The command with its argument boundaries kept, and no tab to break a row.
cmd=$(printf '%q ' "$@"); cmd=${cmd% }; cmd=${cmd//$'\t'/ }
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$run" "$first" "$last" "$ghz" "$ratio" \
  "$cyc" "$tclk" "$busy" "${WTC_ALLOC:-glibc}" "$cmd" >> "$TSV"
exit "$rc"
