#!/bin/sh
#
# Render a few frames of the startup animation, so its timeline can be
# judged without a flash cycle.
#
#   ./launcher/tools/render/scenes/boot_anim_render_host.sh [-o <dir>]
#
# The milliseconds below are three points the animation is recognisably
# different at, not measurements. This script is the standing check that the
# renderer still works at all; --video renders the whole animation.
#
# Everything this does beyond the declarations below is
# tools/render/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=boot_anim
scene_sources="
main/anim/anim_track.c
main/anim/anim_tracks.c
main/asset/asset_pack.c
main/asset/asset_file.c
main/asset/asset_store.c
main/asset/asset_store_file.c
main/gfx/gfx.c
main/util/tune.c
main/boot/boot_anim.c
main/boot/boot_anim_motion.c
tools/render/scenes/boot_anim_render_host.c
"
scene_renders="
early|300|368x448|nopin
middle|1500|368x448|nopin
late|3000|368x448|nopin
"

# The camera and space come from the boot clip's bundle, written here from
# the clip in the tree. Its folder is built into the renderer, which the
# revision comparison runs on its own, so each build finds it beside it;
# AUTANA_ASSET_DIR set to a folder without it renders the rest pose.
# shellcheck source=../../../../scripts/lib/python.sh
. "$SCRIPT_DIR/../../../../scripts/lib/python.sh"
PYTHON=$(find_python) || exit 1
asset_dir="$SCRIPT_DIR/../../results/render/$scene_name/assets"
"$PYTHON" "$SCRIPT_DIR/../../r3d/build_pack.py" -o "$asset_dir" "$SCRIPT_DIR/../../../main/boot" > /dev/null
if command -v cygpath > /dev/null 2>&1; then
    asset_dir=$(cygpath -m "$asset_dir")
fi
scene_defines="-DASSET_DIR_DEFAULT_PATH=\"$asset_dir\""

# shellcheck source=./render_scene.sh
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_run "$@"
