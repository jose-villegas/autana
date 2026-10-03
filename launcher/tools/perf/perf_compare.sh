#!/bin/sh
#
# Compare repeated device performance reports at two revisions.
#
# Usage:
#   launcher/tools/perf/perf_compare.sh [-o DIR] [--runs N] [--timeout SECONDS] [--no-restore] A B -- COMMAND ...
#
# COMMAND runs from this tree. Each detached checkout is supplied through
# --project, followed by its report path. Put --out last when COMMAND writes
# a raw capture; report scripts accept OUT.md.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/../.." && pwd)
REPO_DIR=$(CDPATH= cd -- "$LAUNCHER_DIR/.." && pwd)
PERF_COMPARE_AUTANA=$(command -v autana)

# shellcheck source=../revision_worktree.sh
. "$LAUNCHER_DIR/tools/revision_worktree.sh"

usage() {
    sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

out=""
runs=3
capture_timeout=1800
restore=1
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out=${2:?-o needs a directory}; shift 2 ;;
        --runs) runs=${2:?--runs needs a number}; shift 2 ;;
        --timeout) capture_timeout=${2:?--timeout needs seconds}; shift 2 ;;
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
case "$capture_timeout" in *[!0-9]*|'') usage ;; esac
[ "$capture_timeout" -gt 0 ] || usage
command -v timeout > /dev/null 2>&1 || {
    echo "timeout is required for performance captures" >&2
    exit 1
}
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

# A build id hashes the ELF, so it never names the commit: a capture is
# checked against the id that revision's own build wrote.
expected_build_id() {
    _ebi_file=$(ls -t "$1"/launcher/build*/build_id.txt 2>/dev/null | head -n 1)
    [ -n "$_ebi_file" ] && tr -d '\r\n' < "$_ebi_file"
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
        "$PERF_COMPARE_AUTANA" status > "$out/$_pcs_side/run_${_pcs_run}_status_before.txt" 2>&1 || true
        "$PERF_COMPARE_AUTANA" buildid > "$out/$_pcs_side/run_${_pcs_run}_buildid_before.txt" 2>&1 || true
        if timeout "$capture_timeout" "$@" --project "$_pcs_tree" "$_pcs_report" < /dev/null \
            > "$out/$_pcs_side/run_${_pcs_run}.log" 2>&1; then
            _pcs_status=0
        else
            _pcs_status=$?
        fi
        "$PERF_COMPARE_AUTANA" status > "$out/$_pcs_side/run_${_pcs_run}_status_after.txt" 2>&1 || true
        "$PERF_COMPARE_AUTANA" buildid > "$out/$_pcs_side/run_${_pcs_run}_buildid_after.txt" 2>&1 || true
        if [ "$_pcs_status" -ne 0 ]; then
            capture_failures=$((capture_failures + 1))
            consecutive_failures=$((consecutive_failures + 1))
            if [ "$_pcs_status" -eq 124 ]; then
                _pcs_reason="timed out after ${capture_timeout}s"
            else
                _pcs_reason="failed with exit $_pcs_status"
            fi
            printf '%s\n' "$_pcs_reason" > "$out/$_pcs_side/run_${_pcs_run}.status"
            echo "capture failed: $_pcs_side run $_pcs_run $_pcs_reason" >&2
            if [ "$consecutive_failures" -ge 2 ]; then
                echo "ERROR: stopping after two consecutive capture failures" >&2
                return 1
            fi
            _pcs_run=$((_pcs_run + 1))
            continue
        fi
        _pcs_build=$(tr -d '\r\n' < "$out/$_pcs_side/run_${_pcs_run}_buildid_after.txt")
        _pcs_expected=$(expected_build_id "$_pcs_tree")
        case "$_pcs_build" in *"${_pcs_expected:-no build id written}"*) ;; *)
            capture_failures=$((capture_failures + 1))
            consecutive_failures=$((consecutive_failures + 1))
            printf '%s\n' "build id $_pcs_build is not $_pcs_short's build ${_pcs_expected:-(none written)}" \
                > "$out/$_pcs_side/run_${_pcs_run}.status"
            echo "capture failed: $_pcs_side run $_pcs_run build id $_pcs_build is not $_pcs_short's build ${_pcs_expected:-(none written)}" >&2
            if [ "$consecutive_failures" -ge 2 ]; then
                echo "ERROR: stopping after two consecutive capture failures" >&2
                return 1
            fi
            _pcs_run=$((_pcs_run + 1))
            continue
        esac
        printf '%s\n' "captured $_pcs_build" > "$out/$_pcs_side/run_${_pcs_run}.status"
        printf '%s\n' "$_pcs_report" >> "$out/$_pcs_side/reports.list"
        printf '%s\n' "$_pcs_build" >> "$out/$_pcs_side/buildids.list"
        consecutive_failures=0
        _pcs_run=$((_pcs_run + 1))
    done
}

capture_failures=0
consecutive_failures=0
capture_side a "$rev_a" "$@" || exit 1
capture_side b "$rev_b" "$@" || exit 1
[ "$capture_failures" -eq 0 ] || {
    echo "ERROR: one or more captures failed; no comparison summary was written" >&2
    exit 1
}

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
    "$PERF_COMPARE_AUTANA" flash rel --project "$restore_tree" < /dev/null
fi
echo "summary $out/summary.md"
