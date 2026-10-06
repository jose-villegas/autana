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
main/gfx/gfx.c
main/util/runtime/tune.c
main/boot/boot_anim.c
main/boot/boot_anim_motion.c
tools/render/scenes/boot_anim_render_host.c
"
scene_renders="
early|300|368x448|nopin
middle|1500|368x448|nopin
late|3000|368x448|nopin
"

# The camera and space come from the boot clip's pack; AUTANA_ASSET_DIR set
# to a folder without it renders the rest pose.
scene_assets=main/boot

# shellcheck source=./render_scene.sh
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_run "$@"
