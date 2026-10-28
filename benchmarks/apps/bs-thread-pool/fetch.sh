#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Clones BS::thread_pool at the pinned release into <dir> (read only
# afterwards).
#
#   fetch.sh <dir>
#
# v5.1.0 is bd4533f1f70c2b975cbd5769a60d8eaaea1d2233; the tag is checked
# against that commit, so a moved tag is noticed.
set -euo pipefail
D=${1:?dir}
PIN=bd4533f1f70c2b975cbd5769a60d8eaaea1d2233
if [ ! -d "$D/.git" ]; then
  git -c advice.detachedHead=false clone -q --branch v5.1.0 --depth 1 https://github.com/bshoshany/thread-pool.git "$D"
fi
got=$(git -C "$D" rev-parse HEAD)
[ "$got" = "$PIN" ] || { echo "BS::thread_pool at $got, expected $PIN" >&2; exit 1; }
echo "BS::thread_pool v5.1.0 at $D"
