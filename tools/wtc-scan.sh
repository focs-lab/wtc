#!/usr/bin/env bash
# Run the false-sharing analysis over real translation units and collect what
# it says, one CSV row per finding.
#
#   tools/wtc-scan.sh -o findings.csv file.c [file.cc ...]
#   tools/wtc-scan.sh -o findings.csv -- -I/some/include file.c
#
# Two measured facts shape this script.
#
# ClangIR does not scale to large files the way the ordinary front end does:
# on SQLite's 262,858-line amalgamation it reached 22.9 GiB and had not
# finished after 12 minutes, where `clang -O2 -c` takes 17 seconds and
# 251 MB.  So every compile runs under an explicit memory cap and a timeout,
# and a file that exceeds either is recorded as a refusal rather than being
# allowed to take the host down with it.
#
# And the analysis is whole-program-in-one-translation-unit.  A finding from
# one file of a multi-file program is sound only for objects that file owns;
# the scan records the file so a reader can tell which is which.
set -u

CXX=${WTC_CLANGXX:-$(dirname "$0")/../build-llvm/bin/clang++}
CC=${WTC_CLANG:-$(dirname "$0")/../build-llvm/bin/clang}
OPT=${WTC_OPT:-$(dirname "$0")/../build/bin/wtc-opt}
OUT=findings.csv
MEM=${WTC_SCAN_MEM:-16G}
SECS=${WTC_SCAN_TIMEOUT:-300}
LINE=${WTC_CACHE_LINE:-64}

while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT=$2; shift 2 ;;
    --) shift; break ;;
    *)  break ;;
  esac
done
EXTRA=()
while [ $# -gt 0 ] && [ "${1#-}" != "$1" ]; do EXTRA+=("$1"); shift; done
[ $# -gt 0 ] || { echo "usage: $0 [-o out.csv] [-- cflags...] file..." >&2; exit 2; }

[ -x "$CC" ] || { echo "no clang at $CC" >&2; exit 1; }
[ -x "$CXX" ] || { echo "no clang++ at $CXX" >&2; exit 1; }
[ -x "$OPT" ] || { echo "no wtc-opt at $OPT" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
printf 'file,verdict,object,field_a,field_b,detail\n' > "$OUT"

# Run "$@" under a memory cap and a timeout so a runaway cannot take the host
# down.  `timeout` has to be inside: it execs a binary and cannot run a shell
# function, which is what made the first version refuse every file.
cap () {
  if command -v systemd-run >/dev/null 2>&1; then
    systemd-run --user --scope -q -p MemoryMax="$MEM" -p MemorySwapMax=0 -- \
      timeout "$SECS" "$@"
  else
    timeout "$SECS" "$@"
  fi
}

ok=0; refused=0; findings=0
for src in "$@"; do
  [ -f "$src" ] || continue
  cir=$TMP/$(echo "$src" | tr '/' '_').cir
  # A .c file compiled by the C++ driver is rejected outright by -std=c11,
  # which is how the first run refused all 51 files of memcached.
  case "$src" in *.c) drv=$CC ;; *) drv=$CXX ;; esac
  if ! cap "$drv" -fclangir -emit-cir "${EXTRA[@]+"${EXTRA[@]}"}" \
        "$src" -o "$cir" >"$TMP/cc.log" 2>&1 || [ ! -s "$cir" ]; then
    why=$(grep -m1 -oE 'error: .{0,90}' "$TMP/cc.log" | head -1)
    printf '%s,refused,,,,"front end: %s"\n' "$src" "${why:-no CIR produced within $SECS s / $MEM}" >> "$OUT"
    refused=$((refused+1)); continue
  fi
  if ! cap "$OPT" "$cir" --wtc-false-sharing-diagnose="cache-line=$LINE" \
        >/dev/null 2>"$TMP/opt.log"; then
    printf '%s,refused,,,,"analysis did not finish within %s s / %s"\n' "$src" "$SECS" "$MEM" >> "$OUT"
    refused=$((refused+1)); continue
  fi
  ok=$((ok+1))
  # remark lines carry their own source location, which is what makes a
  # finding comparable against the hand-written annotations in the census.
  n=$(python3 - "$src" "$TMP/opt.log" "$OUT" <<'PY'
import re, sys, csv
src, log, out = sys.argv[1], sys.argv[2], sys.argv[3]
pair = re.compile(r"^(?P<loc>.+?):\s*remark: wtc: (?P<kind>possible false sharing|false sharing) in "
                  r"(?P<obj>\S+): '(?P<a>[^']*)' (?:\(offset \d+\) )?and '(?P<b>[^']*)'")
n = 0
with open(out, "a", newline="") as fh:
    w = csv.writer(fh)
    for line in open(log, errors="replace"):
        m = pair.match(line.rstrip("\n"))
        if not m:
            continue
        verdict = "possible" if m["kind"].startswith("possible") else "definite"
        w.writerow([src, verdict, m["obj"], m["a"], m["b"], m["loc"]])
        n += 1
print(n)
PY
) || n=0
  findings=$((findings + ${n:-0}))
done

printf 'scanned %d, refused %d, findings %d -> %s\n' "$ok" "$refused" "$findings" "$OUT" >&2
