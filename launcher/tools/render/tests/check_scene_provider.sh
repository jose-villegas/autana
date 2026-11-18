#!/bin/sh
# Check provider path hooks with an authored fixture supplied by the caller.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=scene_provider_fixture
scene_provider=
scene_sources="tools/render/tests/scene_provider_fixture.c"
scene_target=render_host
scene_renders=""
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_build "$@"
"$_rs_bin" --scene room -o "$scene_out_dir/hooks.bmp"
# Every weak stand-in in render_host.c is defined strongly in this program, as
# the renderer and scene shell link here: some MinGW linkers leave a call to a
# weak definition unresolved. Needs nm; without it, nothing to read.
if command -v nm >/dev/null 2>&1; then
    symbols=$(nm "$_rs_bin")
    for hook in $(awk '/__attribute__\(\(weak\)\)/ { getline; sub(/\(.*/, ""); print }' "$SCRIPT_DIR/../render_host.c"); do
        if ! printf '%s\n' "$symbols" | grep -Eq " T $hook\$"; then
            echo "check_scene_provider: $hook resolves to render_host.c's weak stand-in" >&2
            exit 1
        fi
    done
fi
