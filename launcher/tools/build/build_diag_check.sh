#!/usr/bin/env bash
#
# Build the DIAGNOSTICS image and run the complexity ratchet: no device,
# nothing flashed.
#
#   tools/build/build_diag_check.sh [--gates-only [--ci]]
#
# --gates-only runs the gates against the build.diag already on disk and
# builds nothing; it is also what CI's Build (Diagnostics) workflow calls
# after its own build, so the gate list below is the only one. --ci makes
# the ratchet score the whole tree and the memory map demand the pinned IDF
# commit.
#
# The result and the log path are printed; the log holds the full output.
#
# Exists because the diagnostics variant is the only one that links every
# test suite into firmware, so it is the only one where a suite's own
# static data is charged against the same internal-heap headroom the shell
# and its apps need. A suite that costs 10 KiB of .bss is invisible to the
# host runner (which has a laptop's memory behind it) and to a release
# build (which links no suites at all), building this variant surfaces it,
# and doing so here beats finding out from CI.
#
# The gates (the complexity ratchet, the stack chain, the MISRA clang-tidy
# count and the rest) are the other half of what CI's Build (Diagnostics)
# workflow decides, so a green build alone settles nothing about a pull
# request; both halves are this one command.
# The ratchet runs BEFORE the build: it costs seconds, the build minutes.
#
# The ratchet reads launcher/build.diag/compile_commands.json, which this
# build writes, so it can only run first once a build.diag exists, with no
# database on disk it runs after the build instead. A database older than a
# first-party .c file or a build-definition file is the previous build's: it
# cannot know a file added since, so the ratchet would name it unmeasured, and
# it runs after the build too.
#
# COMPLEXITY_GATE_BASE overrides the ref the ratchet diffs against
# (default origin/main, the same ref docs/tools/Complexity-Gate.md names
# for local use; --ci scores the whole tree instead).
#
# The build is `autana build diag`, which runs build.sh; ESP-IDF is the one
# selected by espressif.py.

set -euo pipefail

ARGUMENTS=("$@")
GATES_ONLY=0
CI_GATES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --gates-only) GATES_ONLY=1 ;;
        --ci) CI_GATES=1 ;;
        -h|--help) echo "usage: tools/build/build_diag_check.sh [--gates-only [--ci]]"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

# shellcheck disable=SC1007
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck disable=SC1007
REPO_ROOT=$(CDPATH= cd -- "$DIR/../../.." && pwd)
COMPILE_DB="$DIR/../../build.diag/compile_commands.json"
GATE_BASE="${COMPLEXITY_GATE_BASE:-origin/main}"
# shellcheck source=../../../scripts/quiet.sh
. "$DIR/../../../scripts/quiet.sh"

if [ -z "${QUIET_INNER:-}" ]; then
    # Beside build.diag, not in it: the build prepares (and may empty) that folder.
    quiet_begin "$DIR/../../build.diag-check.log"
    quiet_run diagnostics-check env QUIET_INNER=1 bash "$0" ${ARGUMENTS[@]+"${ARGUMENTS[@]}"} || true
    quiet_end build_diag_check || exit $?
    exit 0
fi

# shellcheck source=../../../scripts/lib/python.sh
. "$REPO_ROOT/scripts/lib/python.sh"
PYTHON=$(find_python) || exit 1

complexity_gate() {
    echo "=== Complexity ratchet (--changed $GATE_BASE) ==="
    if [ "$CI_GATES" -eq 1 ]; then
        (cd "$REPO_ROOT" && "$PYTHON" launcher/tools/quality/complexity_gate.py)
    else
        (cd "$REPO_ROOT" && "$PYTHON" launcher/tools/quality/complexity_gate.py \
            --changed "$GATE_BASE")
    fi
}

suite_static_data_gate() {
    # The directory goes in as its own argument so Git Bash converts it for a
    # Windows Python; inside the -c string it would stay a /c/... path.
    IDF_PYTHON=$("$PYTHON" -c "import sys; sys.path.insert(0, sys.argv[1]); from espressif import idf_python; print(idf_python())" "$DIR")
    BUILD_IDF=$("$PYTHON" -c "import json, sys; print(json.load(open(sys.argv[1]))['idf_path'])" "$DIR/../../build.diag/project_description.json")
    "$IDF_PYTHON" "$BUILD_IDF/tools/idf_size.py" --files --format csv "$DIR/../../build.diag/launcher.map" |
        "$PYTHON" "$DIR/../quality/suite_static_data_gate.py"
}

stack_chain_gate() {
    echo "=== Stack chain budget ==="
    "$PYTHON" "$DIR/../quality/stack_chain_gate.py" "$DIR/../../build.diag"
}

build_diag() {
    "$REPO_ROOT/tools/autana" build diag --project "$REPO_ROOT"
}

soft_double_gate() {
    echo "=== Soft-double routines outside logging ==="
    "$PYTHON" "$DIR/check_no_soft_double.py" "$DIR/../../build.diag"
}

psram_tuning_wrap_gate() {
    echo "=== PSRAM tuning off the USB bus ==="
    "$PYTHON" "$DIR/check_psram_tuning_wrap.py" "$DIR/../../build.diag"
}

misra_tidy_gate() {
    echo "=== MISRA clang-tidy count ==="
    (cd "$REPO_ROOT" && "$PYTHON" launcher/tools/quality/misra_tidy_gate.py)
}

iram_budget_gate() {
    echo "=== IRAM placement budgets ==="
    "$PYTHON" "$DIR/memory_map.py" --check $([ "$CI_GATES" -eq 1 ] && echo --require-pin) "$DIR/../../build.diag"
}

# Every gate that reads the build.diag output, in the order they run.
gates() {
    stack_chain_gate
    suite_static_data_gate
    misra_tidy_gate
    soft_double_gate
    psram_tuning_wrap_gate
    iram_budget_gate
}

# True when the database predates a file that decides what the build compiles.
compile_db_stale() {
    local newer
    newer=$(find "$DIR/../../main" "$DIR/../../packages" \
        \( -name '*.c' -o -name CMakeLists.txt -o -name '*.cmake' \) \
        -newer "$COMPILE_DB" -print -quit)
    [ -n "$newer" ] && return 0
    newer=$(find "$DIR/../.." -maxdepth 1 -name 'sdkconfig.defaults*' \
        -newer "$COMPILE_DB" -print -quit)
    [ -n "$newer" ]
}

if [ "$GATES_ONLY" -eq 1 ]; then
    complexity_gate
    gates
    exit 0
fi

if [ -f "$COMPILE_DB" ] && ! compile_db_stale; then
    complexity_gate
    build_diag
    gates
    exit 0
fi

echo "=== No current build.diag compile database - ratchet runs after the build ==="
build_diag
complexity_gate
gates
