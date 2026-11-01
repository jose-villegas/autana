#!/bin/sh
# Checks staged text with the file-scoped commands declared by CI.
set -eu

GATE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$(git rev-parse --show-toplevel)"
# shellcheck source=../lib/python.sh
. "$GATE_DIR/../lib/python.sh"
PYTHON=$(find_python yaml) || exit 1
"$PYTHON" "$GATE_DIR/check_text_rules_staged.py"
