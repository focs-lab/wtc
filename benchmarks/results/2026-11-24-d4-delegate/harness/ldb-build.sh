#!/usr/bin/env bash
# Copy of the script the leg ran, with its paths written as $LEGS (the legs' working
# directory), $PROBES (the probes' material), $WTC_D4 / $WTC_D5 (worktrees at the
# measured commits), $WTC (this tree), $LEG_DIR (the host's leg directory).
# LevelDB's db_bench, the library and the driver as one program, through wtc-cc.
#   ldb-build.sh OUT [wtc-cc options...]
set -eu
S=$PROBES/d4
SRC=${LDB_SRC:-$S/leveldb-src}; OUT=$1; shift
files="db/builder.cc db/c.cc db/db_impl.cc db/db_iter.cc db/dbformat.cc db/dumpfile.cc db/filename.cc db/log_reader.cc db/log_writer.cc db/memtable.cc db/repair.cc db/table_cache.cc db/version_edit.cc db/version_set.cc db/write_batch.cc table/block_builder.cc table/block.cc table/filter_block.cc table/format.cc table/iterator.cc table/merger.cc table/table_builder.cc table/table.cc table/two_level_iterator.cc util/arena.cc util/bloom.cc util/cache.cc util/coding.cc util/comparator.cc util/crc32c.cc util/env.cc util/filter_policy.cc util/hash.cc util/logging.cc util/options.cc util/status.cc util/env_posix.cc util/histogram.cc util/testutil.cc helpers/memenv/memenv.cc benchmarks/db_bench.cc"
args=(); for f in $files; do args+=("$SRC/$f"); done
WTC_ROOT=${WTC_ROOT:-$WTC_D4}
WTC_OPT=${WTC_OPT:-$WTC_ROOT/build/bin/wtc-opt} $WTC_ROOT/tools/wtc-cc --toolchain $WTC/build-llvm "$@" \
  -O2 -fno-exceptions -fno-rtti -DLEVELDB_PLATFORM_POSIX=1 -DLEVELDB_IS_BIG_ENDIAN=0 -DNDEBUG \
  -I"$SRC" -I"$SRC/include" -I"$S/ldb-stub" "${args[@]}" -o "$OUT"
