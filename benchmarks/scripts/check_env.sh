#!/usr/bin/env bash
#
# Refuses to let a measurement run in an environment that would measure the
# neighbours instead of the code. Prints what it found either way, in a form
# campaign.sh copies into meta.json.
#
#   check_env.sh [minimum-cpu-count]
#
# Checks: the governor is "performance" on every allowed CPU, turbo is off,
# and the allowed CPU set has the size the leg expects. Reports the current
# frequency of the first allowed CPU so a campaign can compare before/after.
set -uo pipefail

EXPECT=${1:-}
ALLOWED=$(taskset -pc $$ | sed 's/.*: //')
CPUS=()
IFS=',' read -ra RANGES <<<"$ALLOWED"
for r in "${RANGES[@]}"; do
  if [[ $r == *-* ]]; then for ((i=${r%-*}; i<=${r#*-}; i++)); do CPUS+=("$i"); done; else CPUS+=("$r"); fi
done

fail=0
say() { printf '  %-22s %s\n' "$1" "$2"; }

say "allowed cpus" "$ALLOWED (${#CPUS[@]})"
# A minimum, not an exact count: one reservation can serve legs of different
# widths, and each workload takes the first cores it needs from the set.
if [[ -n $EXPECT && ${#CPUS[@]} -lt $EXPECT ]]; then say "need at least" "$EXPECT cpus: TOO FEW"; fail=1; fi

govs=$(for c in "${CPUS[@]}"; do cat "/sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor" 2>/dev/null; done | sort -u | tr '\n' ' ')
say "governor" "${govs:-unknown}"
[[ $govs == "performance " ]] || { say "governor" "must be performance on every allowed cpu"; fail=1; }

if [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
  nt=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)
  say "intel_pstate/no_turbo" "$nt"
  [[ $nt == 1 ]] || { say "turbo" "must be off"; fail=1; }
elif [[ -r /sys/devices/system/cpu/cpufreq/boost ]]; then
  b=$(cat /sys/devices/system/cpu/cpufreq/boost)
  say "cpufreq/boost" "$b"
  [[ $b == 0 ]] || { say "boost" "must be off"; fail=1; }
else
  say "turbo" "no control file found; cannot verify"; fail=1
fi

f=$(cat "/sys/devices/system/cpu/cpu${CPUS[0]}/cpufreq/scaling_cur_freq" 2>/dev/null || echo unknown)
say "cur freq cpu${CPUS[0]}" "$f kHz"
say "smt (machine-wide)" "$(cat /sys/devices/system/cpu/smt/control 2>/dev/null || echo unknown)"
# What matters is narrower than the machine-wide switch: whether the other
# hardware thread of each reserved core is offline or itself reserved. A
# reservation made with `bench -e` takes siblings offline one by one, which
# smt/control does not show; earlier summaries printed "smt on" for legs
# whose siblings were off. An online sibling outside the set is a
# neighbour on the same core, and fails the check.
foreign=()
for c in "${CPUS[@]}"; do
  sibs=$(cat "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list" 2>/dev/null || echo "$c")
  IFS=',' read -ra parts <<<"${sibs//-/,}"
  for s in "${parts[@]}"; do
    [[ $s == "$c" ]] && continue
    [[ " ${CPUS[*]} " == *" $s "* ]] && continue
    [[ $(cat "/sys/devices/system/cpu/cpu$s/online" 2>/dev/null || echo 1) == 0 ]] && continue
    foreign+=("$s")
  done
done
if (( ${#foreign[@]} )); then
  say "smt siblings" "online outside the set: ${foreign[*]}"; fail=1
else
  say "smt siblings" "offline or reserved"
fi
say "load average" "$(cut -d' ' -f1-3 /proc/loadavg)"

if (( fail )); then echo "  environment NOT acceptable for measurement"; exit 1; fi
echo "  environment acceptable"
