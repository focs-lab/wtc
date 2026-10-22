#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Runs the five programs under the census library and leaves one report
# each in <out-dir>/reports. Untimed; a few seconds of load per program.
#
#   docs/census/run.sh <out-dir> <spdlog-bin-dir> <dedup-bin-dir> <dedup-input>
set -euo pipefail
W=$(cd "${1:?out dir}" && pwd); SP=${2:?spdlog bin dir}; DD=${3:?dedup bin dir}; IN=${4:?dedup input}
HERE=$(cd "$(dirname "$0")" && pwd)
LIB="$W/census/libwtc_threadcensus.so"
R="$W/reports"; mkdir -p "$R"
census() { # report name, command...
  local name=$1; shift
  LD_PRELOAD="$LIB" WTC_CENSUS_OUT="$R/$name.txt" "$@"
}

# memcached: four workers, four client threads, 50k requests each; SIGUSR1
# is memcached's graceful shutdown, which exits and so writes the report.
LD_PRELOAD="$LIB" WTC_CENSUS_OUT="$R/memcached.txt" "$W/memcached/memcached" -t 4 -p 11311 -l 127.0.0.1 -m 64 &
MC=$!; sleep 1
python3 "$HERE/memcached_load.py" 127.0.0.1 11311 4 50000
kill -USR1 $MC; wait $MC || true

# redis: four I/O threads, redis-benchmark with four threads
LD_PRELOAD="$LIB" WTC_CENSUS_OUT="$R/redis.txt" "$W/redis/src/redis-server" --port 6399 --save "" --appendonly no \
  --io-threads 4 --io-threads-do-reads yes --loglevel warning &
RS=$!; sleep 1
redis-benchmark -p 6399 -t set,get -n 200000 -c 50 --threads 4 -q
redis-cli -p 6399 shutdown nosave >/dev/null 2>&1 || true; wait $RS || true

# sqlite: four threads on one serialized connection
census sqlite "$W/sqlite_driver" 4 5000

# spdlog: as released and the compiler's build, four logging threads
census spdlog-orig "$SP/spdlog-orig" --variant orig --threads 4 --n 300000 --payload 64 --sink null
census spdlog-compiler "$SP/spdlog-compiler" --variant compiler --threads 4 --n 300000 --payload 64 --sink null

# dedup: PARSEC's own build, four threads per stage
census dedup "$DD/dedup-orig" -c -p -t 4 -i "$IN" -o /tmp/census-dedup.ddp
rm -f /tmp/census-dedup.ddp
echo "reports in $R"
