#!/usr/bin/env bash
set -euo pipefail

scene=$1
shift
commit=$(git rev-parse HEAD)
printf 'SHIPPED_COMMIT %s\n' "$commit"
autana status
autana flash dev
verify() {
    autana status
    local id
    id=$(autana buildid)
    printf '%s\n' "$id"
    case "$id" in
        "BUILD_ID=${commit:0:12}-dev"*) ;;
        *) return 1 ;;
    esac
}
verify
trap verify EXIT

duration=34
for mode in "$@"; do
    case "$scene" in
        home) autana home ;;
        settings) autana home; autana open settings ;;
        sand) autana home; autana open sand ;;
        water|bistro)
            autana home
            autana open render_lab
            autana render scene "$scene"
            if [ "$scene" = water ]; then
                autana render water tour
            fi
            ;;
        *) exit 2 ;;
    esac
    printf 'PROBE_ARM scene=%s mode=%s\n' "$scene" "$mode"
    log="probe-$scene-$mode.log"
    code=0
    if [ "$mode" = off ]; then
        autana monitor "$duration" --out "$log" || code=$?
    else
        fps=15
        case "$mode" in
            full60) mode=full; fps=60 ;;
            burst) duration=8 ;;
        esac
        autana video -o unused.mp4 --seconds "$duration" --fps "$fps" --probe "$mode" > "$log" 2>&1 || code=$?
    fi
    grep -aE 'PANELCAP_|PROBE_HOST|WATER_PROBE|[0-9] fps,|ms/frame avg/worst|panic|Error|Traceback' "$log" || true
    if [ "$code" != 0 ]; then
        tail -15 "$log"
        exit "$code"
    fi
done
