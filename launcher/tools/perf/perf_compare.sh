#!/bin/sh
#
# Compare repeated device performance reports at two revisions.
#
# Usage:
#   launcher/tools/perf/perf_compare.sh [-o DIR] [--runs N] [--no-restore] A B -- COMMAND ...
#
# COMMAND runs in each detached checkout. Its report path is appended. Put
# --out last when COMMAND writes a raw capture; report scripts accept OUT.md.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/../.." && pwd)
REPO_DIR=$(CDPATH= cd -- "$LAUNCHER_DIR/.." && pwd)

# shellcheck source=../revision_worktree.sh
. "$LAUNCHER_DIR/tools/revision_worktree.sh"

usage() {
    sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

out=""
runs=3
restore=1
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out=${2:?-o needs a directory}; shift 2 ;;
        --runs) runs=${2:?--runs needs a number}; shift 2 ;;
        --no-restore) restore=0; shift ;;
        --) shift; break ;;
        -h|--help) usage ;;
        -*) echo "unknown option: $1" >&2; usage ;;
        *) break ;;
    esac
done
[ $# -ge 3 ] || usage
rev_a=$1
rev_b=$2
shift 2
[ "$1" = -- ] && shift || usage
[ $# -gt 0 ] || usage
case "$runs" in *[!0-9]*|'') usage ;; esac
[ "$runs" -gt 0 ] || usage
[ -n "$out" ] || out=$(mktemp -d)
mkdir -p "$out/a" "$out/b"
out=$(CDPATH= cd -- "$out" && pwd)

work=$(mktemp -d)
revision_worktree_setup "$REPO_DIR" "$work"
cleanup() {
    revision_worktree_cleanup
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

short_name() {
    if [ -d "$1" ]; then basename "$1"; else git -C "$REPO_DIR" rev-parse --short=8 "$1^{commit}"; fi
}

capture_side() {
    _pcs_side=$1
    _pcs_revision=$2
    shift 2
    _pcs_tree=$(revision_worktree_checkout "$_pcs_revision" "$_pcs_side")
    _pcs_short=$(git -C "$_pcs_tree" rev-parse --short=8 HEAD)
    : > "$out/$_pcs_side/reports.list"
    : > "$out/$_pcs_side/buildids.list"
    _pcs_run=1
    while [ "$_pcs_run" -le "$runs" ]; do
        _pcs_report="$out/$_pcs_side/run_$_pcs_run.md"
        (cd "$_pcs_tree" && autana status) > "$out/$_pcs_side/run_${_pcs_run}_status_before.txt" 2>&1
        (cd "$_pcs_tree" && autana buildid) > "$out/$_pcs_side/run_${_pcs_run}_buildid_before.txt" 2>&1 || true
        (cd "$_pcs_tree" && "$@" "$_pcs_report") > "$out/$_pcs_side/run_${_pcs_run}.log" 2>&1
        (cd "$_pcs_tree" && autana status) > "$out/$_pcs_side/run_${_pcs_run}_status_after.txt" 2>&1
        (cd "$_pcs_tree" && autana buildid) > "$out/$_pcs_side/run_${_pcs_run}_buildid_after.txt" 2>&1
        _pcs_build=$(tr -d '\r\n' < "$out/$_pcs_side/run_${_pcs_run}_buildid_after.txt")
        case "$_pcs_build" in *"$_pcs_short"*) ;; *)
            echo "build id $_pcs_build does not identify $_pcs_short" >&2
            return 1
        esac
        printf '%s\n' "$_pcs_report" >> "$out/$_pcs_side/reports.list"
        printf '%s\n' "$_pcs_build" >> "$out/$_pcs_side/buildids.list"
        _pcs_run=$((_pcs_run + 1))
    done
}

capture_side a "$rev_a" "$@"
capture_side b "$rev_b" "$@"

set -- --out "$out/summary.md" --label-a "$(short_name "$rev_a")" --label-b "$(short_name "$rev_b")"
while IFS= read -r item; do set -- "$@" --build-a "$item"; done < "$out/a/buildids.list"
while IFS= read -r item; do set -- "$@" --build-b "$item"; done < "$out/b/buildids.list"
while IFS= read -r item; do set -- "$@" --a "$item"; done < "$out/a/reports.list"
while IFS= read -r item; do set -- "$@" --b "$item"; done < "$out/b/reports.list"
python3 "$TOOLS_DIR/perf_compare.py" "$@"

if grep -q '"sand": true' "$out/comparison.json"; then
    {
        echo
        echo '## Sand report comparison'
        echo
        python3 "$REPO_DIR/launcher/main/apps/sand/tools/compare_reports.py" \
            "$out/a/worst.md" "$out/b/worst.md" || true
    } >> "$out/summary.md"
fi

if [ "$restore" -eq 1 ]; then
    restore_tree=$(revision_worktree_checkout origin/main restore)
    (cd "$restore_tree" && autana flash rel)
fi
echo "summary $out/summary.md"
