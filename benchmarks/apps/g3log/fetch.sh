#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
#
# Clones g3log at the pinned release into <dir> (read only afterwards).
#
#   fetch.sh <dir>
#
# 2.6.2 is f7014e42e48d52db0c4c973658a8027dbf7a576e; the tag is checked
# against that commit, so a moved tag is noticed.
set -euo pipefail
D=${1:?dir}
PIN=f7014e42e48d52db0c4c973658a8027dbf7a576e
if [ ! -d "$D/.git" ]; then
  git clone -q --branch 2.6.2 --depth 1 https://github.com/KjellKod/g3log.git "$D"
fi
got=$(git -C "$D" rev-parse HEAD)
[ "$got" = "$PIN" ] || { echo "g3log at $got, expected $PIN" >&2; exit 1; }
echo "g3log 2.6.2 at $D"
