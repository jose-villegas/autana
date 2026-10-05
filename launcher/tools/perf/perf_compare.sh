#!/bin/sh
# shellcheck disable=SC2034,SC1091,SC2329
# Compare revisions with independent seeded images; restore the release image afterwards.
set -eu
TOOLS_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH='' cd -- "$TOOLS_DIR/../../.." && pwd)
# shellcheck source=../revision_worktree.sh
. "$TOOLS_DIR/../revision_worktree.sh"
rev_a=""
rev_b=""
out=""
restore=1
remaining=$#
while [ "$remaining" -gt 0 ]; do
    arg=$1
    shift
    remaining=$((remaining - 1))
    case "$arg" in
        -o|--out) out=${1:?output directory required}; shift; remaining=$((remaining - 1)) ;;
        --no-restore) restore=0 ;;
        --suite)
            [ "$remaining" -ge 3 ] || exit 2
            name=$1; tests=$2; table=$3
            shift 3; remaining=$((remaining - 3))
            set -- "$@" --suite "$name" "$tests" "$table" ;;
        --threshold|--alpha|--max-seeds|--rng-seed|--timeout|--wait|--autana)
            value=${1:?option value required}; shift; remaining=$((remaining - 1))
            set -- "$@" "$arg" "$value" ;;
        --) ;;
        -h|--help)
            echo 'perf_compare.sh A B --suite NAME TESTS TABLE [-o DIR] [--threshold PCT] [--max-seeds N] [--rng-seed N] [--timeout SECONDS] [--no-restore]'
            exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *)
            if [ -z "$rev_a" ]; then rev_a=$arg
            elif [ -z "$rev_b" ]; then rev_b=$arg
            else echo "unexpected argument: $arg" >&2; exit 2
            fi ;;
    esac
done
[ -n "$rev_a" ] && [ -n "$rev_b" ] || exit 2
[ -n "$out" ] || out=$(mktemp -d)
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)
work=$(mktemp -d)
revision_worktree_setup "$REPO_DIR" "$work"
cleanup() {
    revision_worktree_cleanup
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
tree_a=$(revision_worktree_checkout "$rev_a" a)
tree_b=$(revision_worktree_checkout "$rev_b" b)
status=0
python3 "$TOOLS_DIR/perf_compare.py" --out "$out" --project-a "$tree_a" --project-b "$tree_b" \
    --label-a "$rev_a" --label-b "$rev_b" "$@" || status=$?
if [ "$restore" -eq 1 ]; then
    restore_tree=$(revision_worktree_checkout origin/main restore)
    autana --wait 3600 flash rel --project "$restore_tree" < /dev/null || status=$?
fi
echo "summary $out/summary.md"
exit "$status"
