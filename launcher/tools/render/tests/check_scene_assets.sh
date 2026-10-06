#!/bin/sh
#
# render_scene.sh's scene_assets, checked against a fixture scene: the
# bundles of the declared folders are found through the folder built into the
# renderer, AUTANA_ASSET_DIR still overrides it, and a declared folder with no
# asset roots fails the build instead of rendering without its content.
#
#   ./launcher/tools/render/tests/check_scene_assets.sh
#
# Run by tools/render/render_all_scenes.sh. The fixture is built as a scene is
# (render_scene_build) and run by hand.
#
# POSIX sh, like the rest of this directory.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=asset_fixture
scene_sources="
main/gfx/gfx.c
main/util/tune.c
tools/render/tests/asset_fixture.c
"
scene_renders=""
scene_pin=0
scene_assets=main/boot

# shellcheck source=../render_scene.sh
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_build "$@"

failed=0
log="$scene_out_dir/check.log"
image="$scene_out_dir/check.bmp"
bundle=$(ls "$scene_out_dir/assets" | sed -n 's/\.apak$//p' | head -n 1)

fail() {
    cat "$log" >&2
    echo "FAIL scene assets: $1" >&2
    failed=1
}

if [ -z "$bundle" ]; then
    fail "scene_assets=$scene_assets wrote no bundle"
elif ! "$_rs_bin" "$bundle" -o "$image" > "$log" 2>&1; then
    fail "bundle $bundle was not found in the folder built into the renderer"
else
    echo "ok scene assets: the renderer reads its built-in bundle folder"
fi

empty="$scene_out_dir/no_bundles"
mkdir -p "$empty"
if AUTANA_ASSET_DIR="$(render_scene_to_native "$empty")" "$_rs_bin" "$bundle" -o "$image" > "$log" 2>&1; then
    fail "AUTANA_ASSET_DIR did not override the built-in folder"
else
    echo "ok scene assets: AUTANA_ASSET_DIR overrides the built-in folder"
fi

if (scene_assets=main/gfx render_scene_build > "$log" 2>&1); then
    fail "a scene_assets folder with no asset roots built"
else
    echo "ok scene assets: a folder with no asset roots fails the build"
fi

exit "$failed"
