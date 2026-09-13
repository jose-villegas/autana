#!/usr/bin/env bash
#
# Build the DIAGNOSTICS image and stop - no device, nothing flashed.
#
#   tools/build_diag_check.sh [IDF_EXPORT]
#
# Exists because the diagnostics variant is the only one that links every
# test suite into firmware, so it is the only one where a suite's own
# static data is charged against the same internal-heap headroom the shell
# and its apps need. A suite that costs 10 KiB of .bss is invisible to the
# host runner (which has a laptop's memory behind it) and to a release
# build (which links no suites at all) - building this variant surfaces it,
# and doing so here beats finding out from CI.
#
# See build_flash.sh for the arguments and for why both -D flags on its
# idf.py call are load-bearing.

set -euo pipefail

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$DIR/build_flash.sh" --diag --build-only "$@"
