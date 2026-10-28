#!/usr/bin/env bash
# SPDX-License-Identifier: Zlib
#
# Clones ThreadPool at the pinned commit into <dir> (read only afterwards).
#
#   fetch.sh <dir>
#
# The library has no release tags; the pin is the head of its master
# branch, 9a42ec1329f259a5f4881a291db1dcb8f2ad9040 (26 September 2014),
# the commit every copy of the header in use descends from. The commit is
# checked after the clone, so a rewritten branch is noticed.
set -euo pipefail
D=${1:?dir}
PIN=9a42ec1329f259a5f4881a291db1dcb8f2ad9040
if [ ! -d "$D/.git" ]; then
  git clone -q https://github.com/progschj/ThreadPool.git "$D"
  git -C "$D" -c advice.detachedHead=false checkout -q "$PIN"
fi
got=$(git -C "$D" rev-parse HEAD)
[ "$got" = "$PIN" ] || { echo "ThreadPool at $got, expected $PIN" >&2; exit 1; }
echo "ThreadPool $PIN at $D"
