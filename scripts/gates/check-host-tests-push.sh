#!/bin/sh
# Git supplies every ref update on stdin; check_host_tests_push.py tests once
# if any range changes C or C++ source.
set -eu
GATE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=../lib/python.sh
. "$GATE_DIR/../lib/python.sh"
PYTHON=$(find_python yaml) || exit 1
exec "$PYTHON" "$GATE_DIR/check_host_tests_push.py"
