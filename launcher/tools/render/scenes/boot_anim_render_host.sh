#!/bin/sh
#
# Render a few frames of the startup animation, so its timeline can be
# judged without a flash cycle.
#
#   ./launcher/tools/render/scenes/boot_anim_render_host.sh [-o <dir>]
#
# The milliseconds below are points the animation is recognisably different
# at, not measurements; each one's pixels are pinned in
# boot_anim_render_baseline.txt. --video renders the whole animation.
#
# Everything this does beyond the declarations below is
# tools/render/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

. "$SCRIPT_DIR/launcher_home_render_setup.sh"

scene_name=boot_anim
scene_sources="$launcher_home_sources
main/anim/anim_track.c
main/anim/anim_tracks.c
main/boot/boot_anim.c
main/boot/boot_anim_motion.c
main/boot/boot_anim_photo.c
tools/render/scenes/boot_anim_render_host.c
"
scene_defines="$launcher_home_defines"
scene_renders="
early|300|368x448
middle|1500|368x448
late|3000|368x448
crossfade|3900|368x448
photograph|4500|368x448
"

# The camera and space come from the boot clip's pack and the photograph from
# the boot picture's; AUTANA_ASSET_DIR set to a folder without them renders the
# rest pose without the photograph.
scene_assets=main/boot

# shellcheck source=./render_scene.sh
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_run "$@"
