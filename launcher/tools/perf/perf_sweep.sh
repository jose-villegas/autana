#!/bin/sh
# Sweep console tunables on seeded diagnostic images.
# Usage: perf_sweep.sh --knob NAME=V1,V2 --suite NAME TESTS TABLE [options]
#   --build LABEL=REV        repeatable revision or project directory; default here
#   --knob NAME=V1,V2,...    repeatable; first values are the baseline point
#   --suite NAME TESTS TABLE repeatable; '-' selects all tests or raw timings
#   -o, --out DIR            output directory; default temporary directory
#   --seeds S1 S2 ...        layout seeds; default 1
#   --runs R                rounds per flash; default from perf_sweep.py
#   --rng-seed N             replay point order
#   --threshold PCT         equivalence margin
#   --alpha P               family error rate
#   --timeout SECONDS       capture deadline
#   --wait SECONDS          board queue deadline
#   --autana COMMAND        acquisition and restore command
#   --dry-run               print plan without flashing or restoring
#   --no-restore            omit origin/main release restore
#   -h, --help              print this header

set -eu
TOOLS_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$TOOLS_DIR/../../../scripts/lib/python.sh"
PYTHON=$(find_python) || exit 1
REPO_DIR=$(CDPATH='' cd -- "$TOOLS_DIR/../../.." && pwd)
. "$TOOLS_DIR/../revision_worktree.sh"
work=$(mktemp -d)
revision_worktree_setup "$REPO_DIR" "$work"
restore=1
started=0
dry_run=0
autana=autana
wait=3600
out=""
build_count=0
cleanup() {
    result=$?
    trap - EXIT INT TERM
    if [ "$restore" -eq 1 ] && [ "$started" -eq 1 ] && [ "$dry_run" -eq 0 ]; then
        if restore_tree=$(revision_worktree_checkout origin/main restore); then
            "$PYTHON" "$TOOLS_DIR/perf_compare.py" --restore-project "$restore_tree" \
                --autana "$autana" --wait "$wait" < /dev/null || result=$?
        else
            result=1
        fi
    fi
    revision_worktree_cleanup
    rm -rf "$work"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
remaining=$#
while [ "$remaining" -gt 0 ]; do
    arg=$1
    shift
    remaining=$((remaining - 1))
    case "$arg" in
        --no-restore) restore=0 ;;
        --dry-run) dry_run=1; set -- "$@" "$arg" ;;
        -o|--out) out=${1:?output directory required}; shift; remaining=$((remaining - 1)) ;;
        --build)
            value=${1:?LABEL=REV required}; shift; remaining=$((remaining - 1))
            label=${value%%=*}; revision=${value#*=}
            [ "$label" != "$value" ] && [ -n "$label" ] && [ -n "$revision" ] || {
                echo "--build requires LABEL=REV" >&2; exit 2;
            }
            build_count=$((build_count + 1))
            tree=$(revision_worktree_checkout "$revision" "$build_count")
            set -- "$@" --build "$label=$tree" ;;
        --suite)
            [ "$remaining" -ge 3 ] || { echo "--suite requires NAME TESTS TABLE" >&2; exit 2; }
            name=$1; tests=$2; table=$3
            shift 3; remaining=$((remaining - 3))
            set -- "$@" --suite "$name" "$tests" "$table" ;;
        --knob|--runs|--rng-seed|--threshold|--alpha|--timeout|--wait|--autana)
            value=${1:?option value required}; shift; remaining=$((remaining - 1))
            case "$arg" in
                --wait) wait=$value ;;
                --autana) autana=$value ;;
            esac
            set -- "$@" "$arg" "$value" ;;
        --seeds)
            set -- "$@" --seeds
            count=0
            while [ "$remaining" -gt 0 ]; do
                case "$1" in -*) break ;; esac
                value=$1; shift; remaining=$((remaining - 1))
                set -- "$@" "$value"
                count=$((count + 1))
            done
            [ "$count" -gt 0 ] || { echo "--seeds requires values" >&2; exit 2; } ;;
        -h|--help) sed -n '2,19s/^# //p' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done
[ -n "$out" ] || out=$(mktemp -d)
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)
"$PYTHON" "$TOOLS_DIR/perf_sweep.py" run --out "$out" --dry-run "$@" > "$work/plan.json"
started=1
status=0
"$PYTHON" "$TOOLS_DIR/perf_sweep.py" run --out "$out" "$@" || status=$?
if [ "$dry_run" -eq 0 ]; then echo "table $out/sweep.md"; fi
exit "$status"
