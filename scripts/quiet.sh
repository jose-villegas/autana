#!/bin/sh
#
# Capture a command stream without hiding its result.  The EXIT caller prints
# a verdict because errexit can stop a script before its normal epilogue.

QUIET_LIMIT=30

quiet_begin() {
    QUIET_LOG=$1
    QUIET_STATUS=0
    QUIET_FAILED_LABELS=""
    mkdir -p "$(dirname "$QUIET_LOG")"
    : >"$QUIET_LOG"
    echo "full log: $QUIET_LOG"
}

quiet_run() {
    quiet_label=$1
    shift
    printf '== %s ==\n' "$quiet_label" >>"$QUIET_LOG"

    if [ "${VERBOSE:-0}" -eq 1 ]; then
        printf '== %s ==\n' "$quiet_label"
        quiet_status_file="$QUIET_LOG.$$.status"
        { if "$@"; then echo 0 >"$quiet_status_file"; else echo $? >"$quiet_status_file"; fi; } 2>&1 |
            tee -a "$QUIET_LOG"
        quiet_rc=$(cat "$quiet_status_file")
        rm -f "$quiet_status_file"
    elif "$@" >>"$QUIET_LOG" 2>&1; then
        quiet_rc=0
    else
        quiet_rc=$?
    fi

    if [ "$quiet_rc" -ne 0 ]; then
        QUIET_STATUS=$quiet_rc
        QUIET_FAILED_LABELS="${QUIET_FAILED_LABELS}${QUIET_FAILED_LABELS:+
}$quiet_label"
    fi
    return "$quiet_rc"
}

quiet_error_lines() {
    grep -E 'FAILED:|error:|undefined reference to|out of memory|cannot allocate|CMake Error|ninja: error|^  FAIL |^check_stack_usage: [0-9]+ function\(s\) exceed|^  NEW |^  GREW |AssertionError|^ *File "|^FAIL:|^ERROR:' \
        "$QUIET_LOG" | head -n "$QUIET_LIMIT" || true
}

quiet_failed_sections() {
    printf '%s\n' "$QUIET_FAILED_LABELS" | while IFS= read -r quiet_label; do
        [ -z "$quiet_label" ] || sed -n "\\|^== $quiet_label ==$|,\\|^== |p" "$QUIET_LOG" |
            tail -n "$QUIET_LIMIT"
    done
}

quiet_end() {
    quiet_label=$1
    quiet_status=${2:-$QUIET_STATUS}
    [ "$quiet_status" -eq 0 ] || QUIET_STATUS=$quiet_status

    if [ "$QUIET_STATUS" -eq 0 ]; then
        echo "ok $quiet_label${QUIET_SUMMARY:+: $QUIET_SUMMARY}"
    else
        echo "FAIL $quiet_label${QUIET_SUMMARY:+: $QUIET_SUMMARY}"
        quiet_errors=$(quiet_error_lines)
        if [ -n "$quiet_errors" ]; then
            printf '%s\n' "$quiet_errors" | sed 's/^/  /'
        fi
        quiet_failed_sections | sed 's/^/  /'
        if [ -z "$quiet_errors" ] && [ -z "$QUIET_FAILED_LABELS" ]; then
            tail -n "$QUIET_LIMIT" "$QUIET_LOG" | sed 's/^/  /'
        fi
    fi
    return "$QUIET_STATUS"
}
