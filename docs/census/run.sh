#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Runs the programs under the census library and leaves one report per
# program and load in <out-dir>/reports. Untimed; seconds to a minute of
# load each.
#
#   docs/census/run.sh <out-dir> <spdlog-bin-dir> <dedup-bin-dir> <dedup-inputs> \
#                      <db_bench> <spdlog-census-bin-dir>
#
# <out-dir> is build.sh's, <dedup-inputs> holds native/ and simdev/,
# <db_bench> is build_rocksdb.sh's and <spdlog-census-bin-dir> build_rc.sh's.
set -euo pipefail
W=$(cd "${1:?out dir}" && pwd); SP=${2:?spdlog bin dir}; DD=${3:?dedup bin dir}; IN=${4:?dedup inputs}
DB=${5:?db_bench}; RC=${6:?spdlog census bin dir}
HERE=$(cd "$(dirname "$0")" && pwd)
LIB="$W/census/libwtc_threadcensus.so"
R="$W/reports"; mkdir -p "$R"
census() { # report name, command...
  local name=$1; shift
  LD_PRELOAD="$LIB" WTC_CENSUS_OUT="$R/$name.txt" "$@"
}

# memcached: SIGUSR1 is its graceful shutdown, which exits and so writes the
# report. Four workers and four clients, gets and sets only; eight and
# eight, the same; and eight and eight with one client asking for `stats`
# every 1000 requests.
memcached() { # report, workers, clients, requests, stats every
  LD_PRELOAD="$LIB" WTC_CENSUS_OUT="$R/$1.txt" "$W/memcached/memcached" -t "$2" -p 11311 -l 127.0.0.1 -m 64 &
  local mc=$!; sleep 1
  python3 "$HERE/memcached_load.py" 127.0.0.1 11311 "$3" "$4" ${5:+$5}
  kill -USR1 $mc; wait $mc || true
}
memcached memcached 4 4 50000
memcached memcached-8 8 8 100000
memcached memcached-stats 8 8 100000 1000

# redis: four I/O threads and redis-benchmark with four threads, sets and
# gets; then eight and eight, five commands, a million requests each.
redis() { # report, io threads, benchmark arguments...
  local name=$1 io=$2; shift 2
  LD_PRELOAD="$LIB" WTC_CENSUS_OUT="$R/$name.txt" "$W/redis/src/redis-server" --port 6399 --save "" \
    --appendonly no --io-threads "$io" --io-threads-do-reads yes --loglevel warning &
  local rs=$!; sleep 1
  "$W/redis/src/redis-benchmark" -p 6399 -q "$@"
  "$W/redis/src/redis-cli" -p 6399 shutdown nosave >/dev/null 2>&1 || true; wait $rs || true
}
redis redis 4 -t set,get -n 200000 -c 50 --threads 4
redis redis-heavy 8 -t set,get,incr,lpush,lpop -n 1000000 -c 100 --threads 8

# sqlite: four threads on one serialized connection; then a connection per
# thread to one file
census sqlite "$W/sqlite_driver" 4 5000
census sqlite-per-thread "$W/sqlite_driver" 4 5000 per-thread "$W/census-sqlite.db"
rm -f "$W"/census-sqlite.db*

# spdlog: as released with one worker and with two, and the compiler's build
census spdlog-orig "$SP/spdlog-orig" --variant orig --threads 4 --n 300000 --payload 64 --sink null
census spdlog-orig-2workers "$SP/spdlog-orig" --variant orig --threads 4 --workers 2 --n 300000 --payload 64 --sink null
census spdlog-compiler "$SP/spdlog-compiler" --variant compiler --threads 4 --n 300000 --payload 64 --sink null

# spdlog's reference counts: every operation through wtc-cc --census, as
# released, the port's general build and the compiler's; and releases only,
# through rc_hooks.h, as released
for v in orig generic compiler; do
  census "spdlog-$v-rc" "$RC/spdlog-$v-census" --variant "$v" --threads 4 --n 300000 --payload 64 --sink null
  census "spdlog-$v-rc-2workers" "$RC/spdlog-$v-census" --variant "$v" --threads 4 --workers 2 --n 300000 \
    --payload 64 --sink null
done
census spdlog-orig-rc-hooks "$RC/spdlog-orig-hooks" --variant orig --threads 4 --n 300000 --payload 64 --sink null

# dedup: PARSEC's own build, four threads per stage on the native and the
# simdev input, and eight per stage on the native
dedup() { # report, threads, input
  census "$1" "$DD/dedup-orig" -c -p -t "$2" -i "$3" -o "$W/census-dedup.ddp"
  rm -f "$W/census-dedup.ddp"
}
dedup dedup-native 4 "$IN/native/FC-6-x86_64-disc1.iso"
dedup dedup 4 "$IN/simdev/hamlet.dat"
dedup dedup-native-8 8 "$IN/native/FC-6-x86_64-disc1.iso"

# rocksdb: db_bench, half a million random writes per thread, then random
# reads of that database, with four and with eight threads; the build
# carries rc_hooks.h, so each report has a reference-count section too
for t in 4 8; do
  rm -rf "$W/rocksdb-db"
  census "rocksdb-fillrandom-$t" "$DB" --benchmarks=fillrandom --num=500000 --threads="$t" \
    --db="$W/rocksdb-db" --value_size=100 --compression_type=none --statistics=false
  census "rocksdb-readrandom-$t" "$DB" --benchmarks=readrandom --use_existing_db=1 --num=500000 --reads=500000 \
    --threads="$t" --db="$W/rocksdb-db" --statistics=false
done
rm -rf "$W/rocksdb-db"
python3 "$HERE/symbolize.py" "$R"/*.txt
echo "reports in $R"
