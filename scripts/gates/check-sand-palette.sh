#!/bin/sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
HEADER="$ROOT/launcher/main/apps/sand/sand_palette256.h"
GENERATE="$ROOT/launcher/main/apps/sand/tools/report_shading_palette.sh"
TEMP=$(mktemp -d)
trap 'rm -rf "$TEMP"' EXIT HUP INT TERM

"$GENERATE" "$TEMP/results" "$TEMP/sand_palette256.h"

if ! cmp -s "$HEADER" "$TEMP/sand_palette256.h"; then
    echo "sand_palette256.h differs from report_shading_palette.sh output" >&2
    diff -u "$HEADER" "$TEMP/sand_palette256.h" | head -80 >&2 || true
    exit 1
fi

echo "sand_palette256.h matches report_shading_palette.sh output"
