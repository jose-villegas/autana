#!/bin/sh
# Record a revision's default-image checksum using its span source and suite.
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
git show "$REVISION:launcher/test/suites/suite_r3d_small.c" |
    sed -e 's@"render/r3d_span.h"@"oracle-base.h"@' \
        -e 's@"render/r3d_span.c"@"oracle-base.c"@' > "$SCRATCH/oracle-suite.c"
python - "$SCRATCH" "$RUNNER" <<'PY'
from pathlib import Path
import sys
scratch, runner = sys.argv[1:]
source = Path("launcher/test/run_tests.sh").read_text()
loop = 'for suite_src in "$TEST_DIR"/suites/suite_*.c; do'
replacement = (f'for suite_src in "$TEST_DIR"/suites/suite_*.c "{scratch}/oracle-suite.c"; do\n'
               '    [ "$suite_src" != "$TEST_DIR/suites/suite_r3d_small.c" ] || continue')
if loop not in source:
    raise SystemExit("record_span_oracle: host suite discovery loop not found")
Path(runner).write_text(source.replace(loop, replacement))
PY
bash "$RUNNER" --build-dir "$SCRATCH" --build-only
if grep -q test_old_look_flat_is_bit_identical "$SCRATCH/oracle-suite.c"; then
    pattern=old_look_flat
else
    pattern=defaults
fi
"$SCRATCH/host_tests" --run "run_r3d_small_suite $pattern"
