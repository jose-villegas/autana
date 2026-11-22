#!/bin/sh
#
# Which renderer fill paths each stress preset reaches, counted on a host.
#
#   ./launcher/main/apps/render_lab/tools/stress_path_hits.sh [-o <dir>] [--check]
#
# Builds this app's host render, then draws every run the board's stress
# perf suite makes (each preset at each quarter, --stress-runs) over the
# poses that suite replays (--perf-poses), counting how often each path
# declared in launcher/main/render (render/path/r3d_path.h) was taken
# (--path-hits). Writes the counts as a table, a path a row and a preset a
# column, to stress_path_hits.md in the output folder and to stdout.
#
# --check fails, naming each one, when a path no preset reached: a fill path
# a perf change can move but no stress preset measures. The fix is a preset
# that reaches it.
#
# POSIX sh, like the rest of tools/render.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

out_dir="$SCRIPT_DIR/results/stress_path_hits"
check=0
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out_dir="$2"; shift 2 ;;
        --check) check=1; shift ;;
        *) echo "usage: $0 [-o <dir>] [--check]" >&2; exit 2 ;;
    esac
done
# A run before this one leaves its CSVs, which the table would read.
rm -rf "$out_dir/runs"
mkdir -p "$out_dir/runs"

built=$(sh "$SCRIPT_DIR/render_lab_render_host.sh" -o "$out_dir/build" --build-only | sed -n 's/^built //p')
if [ -z "$built" ]; then
    echo "stress_path_hits: the host render did not build" >&2
    exit 1
fi

# A Windows build prints CRLF lines.
"$built" --stress-runs | tr -d '\r' > "$out_dir/runs.txt"
# No runs, or a run that counted no path, would pass with nothing checked.
if [ ! -s "$out_dir/runs.txt" ]; then
    echo "stress_path_hits: $built --stress-runs listed no stress preset" >&2
    exit 1
fi
while read -r key quarter; do
    run="$out_dir/runs/$key-$quarter"
    if ! "$built" --scene "$key" --quarter "$quarter" --no-hud --perf-poses --dt 0 --path-hits "$run.csv" \
        -o "$run.bmp" < /dev/null > "$run.log" 2>&1; then
        echo "stress_path_hits: $key at quarter $quarter failed; see $run.log" >&2
        exit 1
    fi
    if [ "$(wc -l < "$run.csv")" -le 1 ]; then
        echo "stress_path_hits: $key at quarter $quarter counted no r3d path; is r3d_path linked?" >&2
        exit 1
    fi
done < "$out_dir/runs.txt"

# Each run's CSV, its header skipped, summed per path and preset over the
# quarters; columns in the order the runs came.
table="$out_dir/stress_path_hits.md"
for csv in "$out_dir"/runs/*.csv; do
    run=$(basename "$csv" .csv)
    tail -n +2 "$csv" | sed "s|^|${run%-*},|"
done | awk -F, -v order="$(awk '!seen[$1]++ { printf "%s ", $1 }' "$out_dir/runs.txt")" '
    {
        hits[$2, $1] += $3
        total[$2] += $3
        if (!($2 in total_seen)) { total_seen[$2] = 1; paths[++path_count] = $2 }
    }
    END {
        preset_count = split(order, presets, " ")
        header = "| path |"
        rule = "|---|"
        for (p = 1; p <= preset_count; p++) {
            header = header " " presets[p] " |"
            rule = rule "---|"
        }
        print header " all |"
        print rule "---|"
        for (i = 1; i <= path_count; i++) {
            row = "| " paths[i] " |"
            for (p = 1; p <= preset_count; p++) {
                row = row " " (hits[paths[i], presets[p]] + 0) " |"
            }
            print row " " total[paths[i]] " |"
        }
    }' > "$table"
cat "$table"

unreached=$(awk -F'|' 'NR > 2 && $(NF - 1) + 0 == 0 { gsub(/ /, "", $2); print $2 }' "$table")
if [ "$check" = 1 ] && [ -n "$unreached" ]; then
    for path in $unreached; do
        echo "r3d path $path: no stress preset reaches it; add a preset that does (stress_preset.h)" >&2
    done
    exit 1
fi
