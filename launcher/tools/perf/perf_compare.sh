#!/bin/sh
#
# Compare revisions using independent seeded images; A A calibrates one revision.
# Usage: perf_compare.sh A B --suite NAME TESTS TABLE [options]
#   A and B: revisions or project directories (A A is allowed)
#   --suite NAME TESTS TABLE  repeatable; '-' selects all tests or raw timings
#   -o, --out DIR            output directory
#   --send-a LINE           repeatable setup before each A capture (idempotent sequence)
#   --send-b LINE           repeatable setup before each B capture (idempotent sequence)
#   --threshold PCT         equivalence margin
#   --alpha P               nominal family error rate
#   --max-seeds N           seed cap per side
#   --max-minutes N         stop before a look that would pass this budget
#   --rng-seed N            replay the recorded random plan
#   --timeout SECONDS       capture deadline per run
#   --wait SECONDS          board lock queue time
#   --autana COMMAND        capture, status and restore command (default: PATH)
#   --validate-plan         validate without holding the board
#   --no-restore            omit the origin/main release restore flash
#   -h, --help              print this header

set -eu
TOOLS_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$TOOLS_DIR/../../../scripts/lib/python.sh"
PYTHON=$(find_python) || exit 1
REPO_DIR=$(CDPATH='' cd -- "$TOOLS_DIR/../../.." && pwd)
# shellcheck source=../revision_worktree.sh
# shellcheck disable=SC1091
. "$TOOLS_DIR/../revision_worktree.sh"
rev_a=""
rev_b=""
out=""
restore=1
estimate_restore=""
validate_only=0
perf_job_token=""
restore_tree=""
. "$TOOLS_DIR/perf_job.sh"
autana=autana
wait=3600
remaining=$#
while [ "$remaining" -gt 0 ]; do
    arg=$1
    shift
    remaining=$((remaining - 1))
    case "$arg" in
        -o|--out) out=${1:?output directory required}; shift; remaining=$((remaining - 1)) ;;
        --no-restore) restore=0 ;;
        --validate-plan) validate_only=1 ;;
        --suite)
            [ "$remaining" -ge 3 ] || { echo "--suite requires NAME TESTS TABLE" >&2; exit 2; }
            name=$1; tests=$2; table=$3
            shift 3; remaining=$((remaining - 3))
            set -- "$@" --suite "$name" "$tests" "$table" ;;
        --threshold|--alpha|--max-seeds|--max-minutes|--rng-seed|--timeout|--wait|--autana|--send-a|--send-b)
            value=${1:?option value required}; shift; remaining=$((remaining - 1))
            case "$arg" in
                --autana) autana=$value ;;
                --wait) wait=$value ;;
            esac
            set -- "$@" "$arg" "$value" ;;
        --) ;;
        -h|--help)
            sed -n '3,19s/^# //p' "$0"
            exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *)
            if [ -z "$rev_a" ]; then rev_a=$arg
            elif [ -z "$rev_b" ]; then rev_b=$arg
            else echo "unexpected argument: $arg" >&2; exit 2
            fi ;;
    esac
done
[ -n "$rev_a" ] && [ -n "$rev_b" ] || { echo "two revisions or directories required: A B (or A A)" >&2; exit 2; }
"$PYTHON" "$TOOLS_DIR/perf_compare.py" --validate-plan "$@"
[ -n "$out" ] || out=$(mktemp -d)
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)
work=$(mktemp -d)
revision_worktree_setup "$REPO_DIR" "$work"
# shellcheck disable=SC2329
cleanup() {
    result=$?
    trap - EXIT
    trap '' INT TERM
    if [ -n "$restore_tree" ]; then
        "$PYTHON" "$TOOLS_DIR/perf_compare.py" --restore-project "$restore_tree" \
            --autana "$autana" --wait "$wait" < /dev/null || result=$?
    fi
    perf_job_release || result=$?
    revision_worktree_cleanup
    rm -rf "$work"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
tree_a=$(revision_worktree_checkout "$rev_a" a)
tree_b=$(revision_worktree_checkout "$rev_b" b)
short_name() {
    if [ -d "$1" ]; then printf '%s\n' "$1"
    else git -C "$REPO_DIR" rev-parse --short=8 "$1"
    fi
}
"$PYTHON" "$TOOLS_DIR/perf_compare.py" --validate-plan --project-a "$tree_a" --project-b "$tree_b" "$@"
[ "$validate_only" -eq 0 ] || exit 0
if [ "$restore" -eq 1 ]; then
    restore_project=$(revision_worktree_checkout origin/main restore)
fi
[ "$restore" -eq 1 ] || estimate_restore=--no-restore
estimate=$("$PYTHON" "$TOOLS_DIR/perf_compare.py" --estimate-plan $estimate_restore --project-a "$tree_a" --project-b "$tree_b" "$@")
perf_job_take "$estimate" "perf_compare $(short_name "$rev_a") vs $(short_name "$rev_b")"
if [ "$restore" -eq 1 ]; then restore_tree=$restore_project; fi
status=0
"$PYTHON" "$TOOLS_DIR/perf_compare.py" --out "$out" --project-a "$tree_a" --project-b "$tree_b" \
    --label-a "$(short_name "$rev_a")" --label-b "$(short_name "$rev_b")" "$@" || status=$?
echo "summary $out/summary.md"
exit "$status"
