#!/bin/sh
# Checks staged text with the file-scoped commands declared by CI.
set -eu

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
# shellcheck source=../lib/python.sh
. "$ROOT/scripts/lib/python.sh"
PYTHON=$(find_python yaml) || exit 1
"$PYTHON" "$ROOT/scripts/gates/check_text_rules_staged.py"
