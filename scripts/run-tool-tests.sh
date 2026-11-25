#!/bin/sh
#
# Runs every tooling test file in the repository, each in its own process,
# as many at once as there are CPUs. A tracked test_*.py runs as
# `python -m unittest discover -s <its dir> -p <its name>`, because -s is
# what puts that directory on sys.path, and each suite imports the module
# beside it. Directories with conftest.py or pytest.ini belong to pytest and
# are skipped. Each tracked test_*.mjs runs directly under `node --test`,
# since Node's runner needs no discovery step.
#
# Usage:
#   scripts/run-tool-tests.sh
#   scripts/run-tool-tests.sh --verbose
#   scripts/run-tool-tests.sh --jobs N       (default: the CPU count)
#   scripts/run-tool-tests.sh --shard K/N    (the Kth of N shares, for N runners)
#   scripts/run-tool-tests.sh --list         (print every file a run covers)
#
# The list comes from git on every run, so a test is found wherever it
# lives and build output is never searched. The files that took longest in recent
# runs (run-tool-tests*.times beside the log) start first, then the largest.
# The files a run took are in run-tool-tests.order, or run-tool-tests.K.order
# for shard K, beside the log. A test_*.py file that runs no test fails.
# Exits 1 if any file fails; keeps running the rest so one broken file does
# not hide another. A missing interpreter skips its language's files with a
# warning and still exits 1; unlike scripts/git-hooks/pre-commit, this
# script's job is to prove the suites pass, so a language it could not even
# run is a failure, not something to wave through quietly.
#
# Each file's block in the log ends with its slowest tests' seconds
# (unittest --durations, Python 3.12 on), so a slow file says where its time
# goes.
#
# Default output is the verdict and test count. A passing run can emit
# thousands of characters; the full stream, each file's output in one block
# in a fixed order, is in the printed log path.
#
# POSIX sh, plus xargs -P; .github/workflows/shell-scripts.yml parses it
# under dash.

set -eu

# shellcheck disable=SC1007
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="$REPO_ROOT/launcher/test/build"
QUIET_LOG="$BUILD_DIR/run-tool-tests.log"
UNIT_DIR="$BUILD_DIR/run-tool-tests"

# One file, run by xargs below: its output, status and seconds go under
# UNIT_DIR/files at the file's own path.
if [ "${1:-}" = --unit ]; then
    out="$UNIT_DIR/files/$2"
    mkdir -p "$(dirname -- "$out")"
    started=$(date +%s)
    case "$2" in
        *.mjs) node --test "$REPO_ROOT/$2" ;;
        *) "$PYTHON" -u -m unittest discover ${DURATIONS:+--durations "$DURATIONS"} -s "$REPO_ROOT/$(dirname -- "$2")" -p "$(basename -- "$2")" ;;
    esac >"$out.log" 2>&1 && rc=0 || rc=$?
    # unittest before Python 3.12 passes a file in which it found no test.
    if [ "$rc" -eq 0 ] && grep -aq '^Ran 0 tests' "$out.log"; then
        echo "ERROR: $2 ran no tests" >>"$out.log"
        rc=5
    fi
    echo "$rc" >"$out.status"
    echo "$(($(date +%s) - started)) $2" >"$out.time"
    exit 0
fi

VERBOSE=0
LIST=0
SHARD=1/1
JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
while [ $# -gt 0 ]; do
    case "$1" in
        --verbose) VERBOSE=1 ;;
        --list) LIST=1 ;;
        --jobs | --shard) [ $# -ge 2 ] || { echo "$1 needs a value" >&2; exit 2; }
            if [ "$1" = --jobs ]; then JOBS=$2; else SHARD=$2; fi; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

py_files=""
for file in $(git -C "$REPO_ROOT" ls-files -- ':(glob)**/test_*.py'); do
    dir=$(dirname -- "$file")
    [ ! -f "$REPO_ROOT/$dir/conftest.py" ] && [ ! -f "$REPO_ROOT/$dir/pytest.ini" ] || continue
    py_files="$py_files $file"
done
mjs_files=$(git -C "$REPO_ROOT" ls-files -- ':(glob)**/test_*.mjs')
if [ "$LIST" -eq 1 ]; then
    # shellcheck disable=SC2086
    printf '%s\n' $py_files $mjs_files | sort
    exit 0
fi

if [ -z "$py_files" ] && [ -z "$mjs_files" ]; then
    echo "git tracks no test_*.py or test_*.mjs anywhere in this repository." >&2
    exit 1
fi

# shellcheck source=quiet.sh
. "$REPO_ROOT/scripts/quiet.sh"

# shellcheck source=lib/python.sh
. "$REPO_ROOT/scripts/lib/python.sh"
# shellcheck source=lib/native_path.sh
. "$REPO_ROOT/scripts/lib/native_path.sh"
# shellcheck source=../launcher/tools/build/find_cc.sh
. "$REPO_ROOT/launcher/tools/build/find_cc.sh"
PYTHON=$(find_python 2>/dev/null) || PYTHON=""
# The slowest tests a file's block in the log names; unittest has
# --durations from Python 3.12.
DURATIONS=""
[ -z "$PYTHON" ] || ! "$PYTHON" -c 'import sys; sys.exit(sys.version_info < (3, 12))' 2>/dev/null || DURATIONS=10
export PYTHON UNIT_DIR DURATIONS

quiet_begin "$QUIET_LOG"
# Passing tests print fixture text ("test_clip:FAIL:", argparse "error:"); only
# the runners' own failure headers and assertion lines are failures.
QUIET_ERROR_PATTERN='^FAIL: |^ERROR: |^AssertionError|^not ok |^# fail [1-9]'
export QUIET_STATUS

units=""
if [ -n "$py_files" ]; then
    if [ -z "$PYTHON" ]; then
        echo "python 3 was not found on PATH (tried python3, python, py); skipping test_*.py suites." >&2
        QUIET_STATUS=1
    else
        units="$py_files"
    fi
fi
if [ -n "$mjs_files" ]; then
    if ! command -v node >/dev/null 2>&1; then
        echo "node was not found on PATH; skipping test_*.mjs suites." >&2
        QUIET_STATUS=1
    else
        units="$units $mjs_files"
    fi
fi

rm -rf "$UNIT_DIR"
mkdir -p "$UNIT_DIR"

# Several suites compile the same sources: run_tests.sh's host build, each in
# a build dir of its own, and a scene renderer per suite. With ccache on PATH,
# each compiler they can find (FIND_CC_NAMES, or $CC) is a same-named copy of
# ccache in a folder put first on PATH, ccache's masquerade: a copy runs the
# compiler of its name found next on PATH, so a translation unit compiled once,
# in this run or a cached earlier one, is not compiled again. A $CC given as a
# path gets a copy only where PATH finds that same file. On Windows a copy loads
# DLLs from its own folder first, so the DLLs ccache imports from beside it are
# copied too. If a copy cannot run a compiler, PATH stays as it was. ccache
# waits on a held lock forever, so a cached run goes through ccache_watch.py,
# which stops it naming the lock; the log ends with the cache's statistics.
# CCACHE_NOHASHDIR lets two build dirs share an object; a hit's debug info names
# the first one's folder.
cache_compilers() {
    ccache=$(command -v ccache) || return 0
    masks="$UNIT_DIR/compilers"
    mkdir -p "$masks"
    exe=""
    if [ -f "$ccache.exe" ]; then
        exe=.exe
        home=$(dirname -- "$ccache")
        objdump -p "$ccache.exe" 2>/dev/null | sed -n 's/.*DLL Name: //p' | while read -r dll; do
            [ ! -f "$home/$dll" ] || cp "$home/$dll" "$masks/"
        done
    fi
    cc=""
    cc_mask=""
    # shellcheck disable=SC2086
    for name in ${CC:+"$CC"} $FIND_CC_NAMES; do
        base=$(basename -- "$name" .exe)
        real=$(command -v "$base") || continue
        case "$name" in
            */*)
                # shellcheck disable=SC1007
                found="$(CDPATH= cd -- "$(dirname -- "$real")" && pwd)/$(basename -- "$real" .exe)"
                # shellcheck disable=SC1007
                given="$(CDPATH= cd -- "$(dirname -- "$name")" 2>/dev/null && pwd)/$base"
                [ "$found" = "$given" ] || continue ;;
        esac
        mask="$masks/$base$exe"
        [ -e "$mask" ] || if [ -n "$exe" ]; then cp "$ccache.exe" "$mask"; else ln -s "$ccache" "$mask"; fi
        [ -n "$cc" ] || cc=$mask
        [ "$name" != "${CC:-}" ] || cc_mask=$mask
    done
    [ -n "$cc" ] && CCACHE_DISABLE=1 "$cc" --version >/dev/null 2>&1 || return 0
    PATH="$masks:$PATH"
    [ -z "$cc_mask" ] || CC=$(to_native "$cc_mask")
    WATCH=$(to_native "$REPO_ROOT/launcher/tools/build/ccache_watch.py")
    CCACHE_NOHASHDIR=${CCACHE_NOHASHDIR:-1}
    export PATH CCACHE_NOHASHDIR ${CC:+CC}
}
WATCH=""
[ -z "$PYTHON" ] || cache_compilers
# This run's seconds and files; a shard's go in files of its own, and every
# seconds file is read.
case "$SHARD" in 1/1) RUN="$BUILD_DIR/run-tool-tests" ;; *) RUN="$BUILD_DIR/run-tool-tests.${SHARD%/*}" ;; esac
# Longest first: last run's seconds, then bytes, which order a first run. Of
# N shards, each file goes to the one with the fewest seconds so far (a file
# never timed counts one), and shard K runs its own.
# shellcheck disable=SC2086
[ -z "$units" ] || { cat "$BUILD_DIR"/run-tool-tests*.times 2>/dev/null || :; (cd "$REPO_ROOT" && wc -c $units) | sed 's/^ */+ /'; } |
    awk '$1 != "+" { seconds[$2] = $1; next } $3 != "total" { print seconds[$3] + 0, $2, $3 }' |
    sort -k1,1nr -k2,2nr | tee "$UNIT_DIR/known" |
    awk -v shard="$SHARD" 'BEGIN { split(shard, s, "/") }
        { least = 1; for (k = 2; k <= s[2]; k++) if (load[k] < load[least]) least = k
          load[least] += $1 > 1 ? $1 : 1; if (least == s[1]) print $3 }' >"$UNIT_DIR/order"
if [ ! -s "$UNIT_DIR/order" ]; then :
elif [ -n "$WATCH" ]; then "$PYTHON" "$WATCH" -- xargs -P "$JOBS" -n 1 sh "$0" --unit <"$UNIT_DIR/order"
else xargs -P "$JOBS" -n 1 sh "$0" --unit <"$UNIT_DIR/order"
fi
sort "$UNIT_DIR/order" >"$RUN.order"

: >"$UNIT_DIR/times"
unit_count=0
while IFS= read -r file; do
    unit_count=$((unit_count + 1))
    out="$UNIT_DIR/files/$file"
    printf '== %s ==\n' "$file" >>"$QUIET_LOG"
    cat "$out.log" >>"$QUIET_LOG" 2>/dev/null || true
    cat "$out.time" >>"$UNIT_DIR/times" 2>/dev/null || true
    quiet_record "$file" "$(cat "$out.status" 2>/dev/null || echo 1)"
done <"$RUN.order"
[ -z "$WATCH" ] || { echo "== ccache =="; ccache --show-stats; } >>"$QUIET_LOG" 2>&1 || :
# A file's time depends on what ran beside it, so what is kept is the mean with
# last run's, not this run's alone, or the shares swing from run to run.
awk 'NR == FNR { known[$3] = $1; next } { print known[$2] ? int((known[$2] + $1 + 1) / 2) : $1, $2 }' \
    "$UNIT_DIR/known" "$UNIT_DIR/times" >"$RUN.times"
[ "$VERBOSE" -eq 0 ] || cat "$QUIET_LOG"

py_tests=$(grep -aoE '^Ran [0-9]+ tests? in' "$QUIET_LOG" | grep -oE '[0-9]+' | { sum=0; while read -r n; do sum=$((sum + n)); done; echo "$sum"; })
node_tests=$(grep -aoE 'tests [0-9]+$' "$QUIET_LOG" | grep -oE '[0-9]+' | { sum=0; while read -r n; do sum=$((sum + n)); done; echo "$sum"; })
QUIET_SUMMARY="$((py_tests + node_tests)) tests across $unit_count files on $JOBS jobs"
export QUIET_SUMMARY
quiet_end run-tool-tests
