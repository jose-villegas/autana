#!/bin/sh
# Record the default-image checksum using a revision's span source and header.
# Run through quiet.py; a checksum assertion failure reports the recorded value.
# Usage: bash launcher/tools/render/record_span_oracle.sh REVISION
set -eu
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
REVISION=${1:?revision required}
mkdir -p "$ROOT/launcher/test/build"
SCRATCH=$(mktemp -d "$ROOT/launcher/test/build/span-oracle.XXXXXX")
RUNNER=$(mktemp "$ROOT/launcher/test/.span-oracle.XXXXXX.sh")
trap 'rm -f "$RUNNER"' EXIT HUP INT TERM
git show "$REVISION:launcher/main/render/r3d_span.h" > "$SCRATCH/oracle-base.h"
git show "$REVISION:launcher/main/render/r3d_span_internal.h" |
    sed 's@"render/r3d_span.h"@"oracle-base.h"@' > "$SCRATCH/oracle-internal.h"
git show "$REVISION:launcher/main/render/r3d_span.c" |
    sed -e 's@"render/r3d_span.h"@"oracle-base.h"@' \
        -e 's@"render/r3d_span_internal.h"@"oracle-internal.h"@' > "$SCRATCH/oracle-base.c"
sed "s@\$MAIN_DIR/render/r3d_span.c@$SCRATCH/oracle-base.c@" launcher/test/run_tests.sh > "$RUNNER"
bash "$RUNNER" --build-dir "$SCRATCH" --build-only
"$SCRATCH/host_tests" --run 'run_r3d_small_suite defaults'
