#!/bin/sh
#
# Compare repeated device performance reports at two revisions.
#
# Usage:
#   launcher/tools/perf/perf_compare.sh [-o DIR] [--runs N] [--timeout SECONDS] [--no-restore] A B -- COMMAND ...
#
# COMMAND runs from this tree. Each detached checkout is supplied through
# --project. A COMMAND that writes a raw capture names its destination @CAPTURE@
# and the numbers are read from that file, never from the report beside it.
# Without @CAPTURE@ the report path is appended, for scripts that accept OUT.md.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/../.." && pwd)
REPO_DIR=$(CDPATH= cd -- "$LAUNCHER_DIR/.." && pwd)
PERF_COMPARE_AUTANA=$(command -v autana)

# shellcheck source=../revision_worktree.sh
. "$LAUNCHER_DIR/tools/revision_worktree.sh"

usage() {
    sed -n '3,11p' "$0" | sed 's/^# \{0,1\}//' >&2
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

valid_build_id() {
    case "$1" in
        ''|*[!A-Za-z0-9._-]*) return 1 ;;
        *-*) return 0 ;;
        *) return 1 ;;
    esac
}

build_id_from_reply() {
    sed -n 's/^BUILD_ID=\([A-Za-z0-9._-]*\)$/\1/p' "$1" | tail -n 1
}

# @CAPTURE@ in COMMAND becomes this run's raw capture path; a COMMAND without
# one is given the report path last. Rotating "$@" here keeps the caller's own
# copy of COMMAND intact for the next run.
run_command() {
    _rc_tree=$1
    _rc_capture=$2
    _rc_report=$3
    shift 3
    _rc_named=0
    _rc_count=$#
    while [ "$_rc_count" -gt 0 ]; do
        _rc_arg=$1
        shift
        case "$_rc_arg" in
            *"@CAPTURE@"*)
                _rc_named=1
                _rc_arg="${_rc_arg%%"@CAPTURE@"*}$_rc_capture${_rc_arg#*"@CAPTURE@"}" ;;
        esac
        set -- "$@" "$_rc_arg"
        _rc_count=$((_rc_count - 1))
    done
    if [ "$_rc_named" -eq 1 ]; then
        timeout "$capture_timeout" "$@" --project "$_rc_tree" < /dev/null
    else
        timeout "$capture_timeout" "$@" --project "$_rc_tree" "$_rc_report" < /dev/null
    fi
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
        _pcs_capture="$out/$_pcs_side/run_$_pcs_run.capture.log"
        _pcs_numbers=$_pcs_report
        case " $* " in *"@CAPTURE@"*) _pcs_numbers=$_pcs_capture ;; esac
        "$PERF_COMPARE_AUTANA" status > "$out/$_pcs_side/run_${_pcs_run}_status_before.txt" 2>&1 || true
        if run_command "$_pcs_tree" "$_pcs_capture" "$_pcs_report" "$@" \
            > "$out/$_pcs_side/run_${_pcs_run}.log" 2>&1; then
            _pcs_status=0
        else
            _pcs_status=$?
        fi
        "$PERF_COMPARE_AUTANA" status > "$out/$_pcs_side/run_${_pcs_run}_status_after.txt" 2>&1 || true
        _pcs_status_note=""
        if [ "$_pcs_status" -ne 0 ]; then
            _pcs_complete=1
            if [ "$_pcs_numbers" = "$_pcs_capture" ] \
                    && ! grep -q '^- Ended: complete' "${_pcs_capture%.*}.md" 2>/dev/null; then
                _pcs_complete=0
            fi
            if [ "$_pcs_status" -eq 1 ] \
                    && [ "$_pcs_complete" -eq 1 ] \
                    && python3 "$TOOLS_DIR/perf_compare.py" --check-report "$_pcs_numbers"; then
                _pcs_status_note="; command exit 1 recorded a test failure"
                echo "capture kept: $_pcs_side run $_pcs_run has timing rows despite a failing test" >&2
            else
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
        fi
        # The flash logs the id it booted while it still holds the board; a
        # `buildid` taken after the lock is released may meet another owner.
        _pcs_build=$(sed -n 's/^booted BUILD_ID=\([^ ]*\).*/\1/p' "$out/$_pcs_side/run_${_pcs_run}.log" \
            | tail -n 1 | tr -d '\r')
        if ! valid_build_id "$_pcs_build"; then
            "$PERF_COMPARE_AUTANA" --wait "$capture_timeout" buildid \
                > "$out/$_pcs_side/run_${_pcs_run}_buildid_after.txt" 2>&1 || true
            _pcs_build=$(build_id_from_reply "$out/$_pcs_side/run_${_pcs_run}_buildid_after.txt")
        fi
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
        printf '%s\n' "captured $_pcs_build$_pcs_status_note" > "$out/$_pcs_side/run_${_pcs_run}.status"
        printf '%s\n' "$_pcs_numbers" >> "$out/$_pcs_side/reports.list"
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
