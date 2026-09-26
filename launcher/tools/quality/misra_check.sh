#!/bin/sh
#
# Runs Cppcheck's MISRA C:2012 addon (plus its native defect checks) over
# app source, using a real ESP-IDF compile_commands.json so include paths
# and macros resolve correctly - a standalone run without one produces
# mostly noise (unresolved types make almost everything look like a 10.x
# essential-type violation).
#
# Report-only about its FINDINGS: any number of them still exits 0. A refused
# argument, a timeout or a cppcheck failure exits non-zero, so a report
# consumer cannot patch from a truncated report. The whole
# of main/ currently turns up ~6400 MISRA findings of its own (dominated by
# 12.1, 10.4 and 15.5 - see below), so gating CI on this before triage
# would just be a wall no one reads. Flip EXIT_ON_FINDINGS below once a
# rule set has been chosen and the backlog triaged.
#
# Usage:
#   tools/quality/misra_check.sh [build_dir] [file_filter]
#
#   build_dir     defaults to build.dev - must have compile_commands.json
#                 (run `tools/build/build_flash.sh --build-only` first if it doesn't)
#   file_filter   cppcheck --file-filter glob, defaults to the sand app
#                 (pass "*/main/*" for the whole project, minus vendored
#                 dependencies)
#
#   MISRA_JOBS             parallel analyses, defaults to 2 and may not
#                          exceed 4 (each job also runs the Python addon)
#   MISRA_TIMEOUT_SECONDS  whole-scan deadline, defaults to 900 seconds
#   MISRA_FORCE            0 (default) analyses the configuration build_dir
#                          pins; 1 also tries every first-party #ifdef
#                          combination, several times slower
#   MISRA_MAX_UNITS        translation units the filter may match, defaults
#                          to 150 (the whole project under main/ is ~80)
#
# NEVER pass a bare "*". --file-filter controls what cppcheck actually
# ANALYZES, not just what gets reported -- "*" matches every translation
# unit in compile_commands.json, well over a thousand on this repo and
# nearly all of them vendored ESP-IDF code, none of it main/ code worth a
# finding. That runs cppcheck's MISRA addon over the whole SDK for nothing,
# past 12GB of RAM, with no end in sight. "*/main/*" scopes
# analysis to the ~80 real translation units under main/ instead -- use
# that for a whole-project scan, not "*". The filter is checked for shape
# and then the matches are counted, so a filter that reaches that far is
# refused before cppcheck starts rather than discovered an hour in.
#
# Output goes to tools/results/misra_<file_filter-ish>.txt (gitignored).

set -eu

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_DIR="$(cd "$TOOLS_DIR/../.." && pwd)"

BUILD_DIR="${1:-build.dev}"
FILE_FILTER="${2:-*/main/apps/sand/*}"
JOBS="${MISRA_JOBS:-2}"
TIMEOUT_SECONDS="${MISRA_TIMEOUT_SECONDS:-900}"
MAX_UNITS="${MISRA_MAX_UNITS:-150}"


# Anchored at the front, so "*/managed_components/*/main/*" cannot pass by
# merely containing the word. The filter's shape is only half the guard
# though - it is matched against whatever absolute paths the compile database
# holds, so a checkout living under a directory called main/ would satisfy
# any spelling of this. What is actually analysed is counted below, and that
# count is what refuses to run.
case "$FILE_FILTER" in
    '*/main/'*) ;;
    *)
        echo "Refusing unsafe file filter '$FILE_FILTER'. It must start with '*/main/'." >&2
        exit 2
        ;;
esac

case "$JOBS" in
    ''|*[!0-9]*)
        echo "MISRA_JOBS must be an integer from 1 to 4." >&2
        exit 2
        ;;
esac
if [ "$JOBS" -lt 1 ] || [ "$JOBS" -gt 4 ]; then
    echo "MISRA_JOBS must be from 1 to 4; each job also runs the Python addon." >&2
    exit 2
fi

case "$TIMEOUT_SECONDS" in
    ''|*[!0-9]*)
        echo "MISRA_TIMEOUT_SECONDS must be a positive integer." >&2
        exit 2
        ;;
esac
if [ "$TIMEOUT_SECONDS" -lt 1 ]; then
    echo "MISRA_TIMEOUT_SECONDS must be a positive integer." >&2
    exit 2
fi

COMPILE_COMMANDS="$LAUNCHER_DIR/$BUILD_DIR/compile_commands.json"
if [ ! -f "$COMPILE_COMMANDS" ]; then
    echo "No compile_commands.json in $BUILD_DIR/ - run 'tools/build/build_flash.sh --build-only' first." >&2
    exit 1
fi

# One configuration per scan: the one this build directory's sdkconfig.h
# pins. Another variant is scanned through its own build directory (build/
# release, build.diag/ diagnostics). --force instead invents combinations no
# sdkconfig.h produces, DEVELOPMENT and RELEASE both set, and took
# app_sand.c from 58 s to past 600 s. The vendored trees stay pinned even then.
FORCE_FLAG=""
if [ "${MISRA_FORCE:-0}" = "1" ]; then
    idf_dir="$(grep -o '[A-Za-z]:[^" ]*esp-idf\|/[^" ]*esp-idf' "$COMPILE_COMMANDS" | head -1)"
    FORCE_FLAG="--force --config-exclude=$LAUNCHER_DIR/components --config-exclude=$LAUNCHER_DIR/managed_components"
    if [ -n "$idf_dir" ]; then
        FORCE_FLAG="$FORCE_FLAG --config-exclude=$idf_dir"
    fi
fi

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "cppcheck not found on PATH. It's free/GPLv3 - install it:" >&2
    echo "  Windows: winget install cppcheck" >&2
    echo "  macOS:   brew install cppcheck" >&2
    echo "  Linux:   apt-get install cppcheck (or your distro's package)" >&2
    exit 1
fi

# The misra.py addon ships inside Cppcheck's install but isn't always on a
# path cppcheck resolves by name alone (e.g. the winget/mingw64 package on
# Windows). Search common share/ locations before giving up on plain
# "misra" and letting cppcheck fail with its own "addon not found" error.
ADDON="misra"
for candidate in \
    "$(command -v cppcheck 2>/dev/null | sed 's|/bin/cppcheck.*||')/share/Cppcheck/addons/misra.py" \
    /usr/share/cppcheck/addons/misra.py \
    /usr/local/share/Cppcheck/addons/misra.py
do
    if [ -f "$candidate" ]; then
        ADDON="$candidate"
        break
    fi
done

RESULTS_DIR="$TOOLS_DIR/../results"
mkdir -p "$RESULTS_DIR"
safe_name="$(echo "$FILE_FILTER" | tr -c 'A-Za-z0-9_' '_')"
REPORT="$RESULTS_DIR/misra_${safe_name}.txt"

if command -v timeout >/dev/null 2>&1; then
    TIMEOUT=timeout
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT=gtimeout
else
    echo "GNU timeout is required so a bad addon rule cannot run indefinitely." >&2
    echo "  macOS: brew install coreutils" >&2
    exit 1
fi

echo "Scanning '$FILE_FILTER' against $BUILD_DIR/compile_commands.json ($JOBS jobs, ${TIMEOUT_SECONDS}s timeout)..."
cd "$LAUNCHER_DIR"

# A compile database lists the files that existed when it was generated, and
# cppcheck only complains when the filter matches NOTHING - so a file added
# since the last build is skipped in silence and the report reads as a clean
# one. material_palette.c was a week old and in no build directory here.
# Naming what will actually be analysed is the cheapest way to notice.
matched="$(grep -o '"file": *"[^"]*"' "$COMPILE_COMMANDS" |
    sed 's|.*"file": *"||; s|"$||; s|\\\\|/|g' | sort -u |
    while read -r entry; do
        case "$entry" in
            $FILE_FILTER) basename "$entry" ;;
        esac
    done)"
count="$(printf '%s' "$matched" | grep -c . || true)"
echo "Translation units in $BUILD_DIR/compile_commands.json matching the filter: ${count:-0}"
if [ "${count:-0}" -gt 0 ] && [ "${count:-0}" -le 20 ]; then
    echo "$matched" | sed 's|^|  |'
    echo "  (a source you expected and cannot see here is missing from the build - rebuild)"
fi

# The count, not the glob, is what stops a runaway: whatever the filter says,
# this is the number of translation units about to be analysed, and the
# incident behind this script was ~1800 of them. The whole project under
# main/ is around 80.
if [ "${count:-0}" -gt "$MAX_UNITS" ]; then
    echo "Refusing to scan $count translation units (limit $MAX_UNITS)." >&2
    echo "The filter '$FILE_FILTER' is reaching outside this project's main/." >&2
    echo "Raise MISRA_MAX_UNITS deliberately if the project really is that big now." >&2
    exit 2
fi

# Any file that stubs a macro for the analyser is analysed as stubbed, not as
# written. Name them, so a finding count is never read as full coverage.
stubbed="$(grep -rl --include='*.c' --include='*.h' --exclude-dir=tools 'ANALYSIS_SCAN' main 2>/dev/null || true)"
if [ -n "$stubbed" ]; then
    echo "Analysed with source-level stubs (see each ANALYSIS_SCAN block for scope):"
    echo "$stubbed" | sed 's|^|  |'
fi
# The database by absolute path: given a relative one, Cppcheck 2.13 keeps
# each unit's path relative to the working directory ("main/..."), and the
# '*/main/' filter above then matches nothing.
set +e
"$TIMEOUT" --signal=TERM --kill-after=10s "${TIMEOUT_SECONDS}s" cppcheck \
    --project="$COMPILE_COMMANDS" \
    --file-filter="$FILE_FILTER" \
    -DANALYSIS_SCAN=1 \
    $FORCE_FLAG \
    -j "$JOBS" \
    --enable=warning,style,performance,portability \
    --addon="$ADDON" \
    --inline-suppr \
    --suppress=missingIncludeSystem \
    -q \
    --output-file="$REPORT" 2>&1
scan_status=$?
set -e

# The addon deletes each unit's .dump when it finishes, so only a killed scan
# leaves them, beside the sources: gigabytes each, one per unit and process.
find main -name '*.dump' -delete 2>/dev/null || true
rm -f ./*.ctu-info

if [ "$scan_status" -eq 124 ] || [ "$scan_status" -eq 137 ]; then
    echo "Cppcheck exceeded the ${TIMEOUT_SECONDS}s deadline; partial report: $REPORT" >&2
    exit 2
fi
if [ "$scan_status" -ne 0 ]; then
    echo "Cppcheck failed with exit $scan_status; partial report: $REPORT" >&2
    exit "$scan_status"
fi

total="$(wc -l <"$REPORT" | tr -d ' ')"
misra_count="$(grep -c 'misra-c2012-' "$REPORT" 2>/dev/null || true)"
other_count="$(grep -cE ': (error|warning):' "$REPORT" 2>/dev/null || true)"

# Findings inside ESP-IDF and vendored headers are reported too, and on a
# whole-project scan they are nearly half the total (mostly 2.5, unused macros).
own="$(grep 'misra-c2012-' "$REPORT" 2>/dev/null | grep -E '[\\/]launcher[\\/]main[\\/]' || true)"
own_count="$(printf '%s' "$own" | grep -c . || true)"

echo ""
echo "Report: $REPORT ($total lines)"
echo "  MISRA findings:        ${misra_count:-0} (${own_count:-0} in main/)"
echo "  native error/warning:  ${other_count:-0}"
echo ""
echo "Top MISRA rules hit in main/:"
printf '%s\n' "$own" | grep -oE 'misra-c2012-[0-9.]+' | sort | uniq -c | sort -rn | head -10 || true
