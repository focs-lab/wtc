#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Clones spdlog at the pinned release into <dir> (read only afterwards).
#
#   fetch.sh <dir>
#
# v1.14.1 is 27cb4c76708608465c413f6d0e6b8d99a4d84302; the tag is checked
# against that commit, so a moved tag is noticed.
set -euo pipefail
D=${1:?dir}
PIN=27cb4c76708608465c413f6d0e6b8d99a4d84302
if [ ! -d "$D/.git" ]; then
  git clone -q --branch v1.14.1 --depth 1 https://github.com/gabime/spdlog.git "$D"
fi
got=$(git -C "$D" rev-parse HEAD)
[ "$got" = "$PIN" ] || { echo "spdlog at $got, expected $PIN" >&2; exit 1; }
echo "spdlog v1.14.1 at $D"
