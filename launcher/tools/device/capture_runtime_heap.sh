#!/usr/bin/env bash
# Capture each build's internal heap after boot and after opening each app.
set -euo pipefail

AUTANA=${AUTANA:-autana}
readonly SETTLE_SECONDS=2
readonly BOOT_TIMEOUT_SECONDS=120
readonly BOOT_RETRY_SECONDS=2
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
if [[ $# != 1 ]]; then
    echo "usage: capture_runtime_heap.sh OUTPUT" >&2
    exit 2
fi
mkdir -p "$(dirname "$1")"
OUTPUT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
CAPTURE=$(mktemp "${OUTPUT}.XXXXXX")
trap 'rm -f "$CAPTURE"' EXIT
cd "$ROOT"

wait_build() {
    local expected=$1 deadline=$((SECONDS + BOOT_TIMEOUT_SECONDS)) actual
    while (( SECONDS < deadline )); do
        if actual=$("$AUTANA" --wait 3600 buildid --json); then
            actual=$(printf '%s' "$actual" | python -c 'import json,sys; print(json.load(sys.stdin)["build_id"])')
            if [[ "$actual" == "$expected" ]]; then
                return
            fi
        fi
        sleep "$BOOT_RETRY_SECONDS"
    done
    echo "build id did not match $expected before the boot deadline" >&2
    return 1
}

check_build() {
    local expected=$1 actual
    "$AUTANA" --wait 3600 status >&2
    actual=$("$AUTANA" --wait 3600 buildid --json)
    actual=$(printf '%s' "$actual" | python -c 'import json,sys; print(json.load(sys.stdin)["build_id"])')
    if [[ "$actual" != "$expected" ]]; then
        echo "capture discarded: expected $expected, got $actual" >&2
        return 1
    fi
}

RELEASE_ID=$(tr -d '\r\n' < launcher/build/build_id.txt)
printf 'BUILD_ID=%s\nunmeasured: release has no console, and its boot log is lost in the USB reset gap\n' "$RELEASE_ID" > "$CAPTURE"
for variant in dev diag; do
    "$AUTANA" --wait 3600 flash "$variant" >&2
    expected=$(tr -d '\r\n' < "launcher/build.$variant/build_id.txt")
    wait_build "$expected"
    check_build "$expected"
    sleep "$SETTLE_SECONDS"
    reading=$("$AUTANA" --wait 3600 debug heap --json)
    printf '\nBUILD_ID=%s\nphase=boot %s\n' "$expected" "$reading" >> "$CAPTURE"
    apps=$("$AUTANA" --wait 3600 apps --json)
    # A Windows python prints CRLF; the names must not carry the CR into the capture.
    names=$(printf '%s' "$apps" | python -c 'import json,sys; print("\n".join(app["name"] for app in json.load(sys.stdin)["apps"]))' | tr -d '\r')
    while IFS= read -r app; do
        [[ -n "$app" ]] || continue
        "$AUTANA" --wait 3600 open "$app" >&2
        sleep "$SETTLE_SECONDS"
        reading=$("$AUTANA" --wait 3600 debug heap --json)
        printf 'phase=app:%s %s\n' "$app" "$reading" >> "$CAPTURE"
        "$AUTANA" --wait 3600 home >&2
    done <<< "$names"
    check_build "$expected"
done
mv "$CAPTURE" "$OUTPUT"
trap - EXIT
echo "Runtime heap capture: $OUTPUT"
