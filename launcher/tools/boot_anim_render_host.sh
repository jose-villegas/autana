#!/bin/sh
#
# Render a few frames of the startup animation, so its timeline can be
# judged without a flash cycle.
#
#   ./launcher/tools/boot_anim_render_host.sh [-o <dir>]
#
# The milliseconds below are three points the animation is recognisably
# different at, not measurements. tools/boot_anim_editor_server.py drives
# the same binary at whatever millisecond its browser asks for; this script
# is the standing check that the renderer still works at all.
#
# Everything this does beyond the declarations below is
# tools/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=boot_anim
scene_sources="
main/gfx/gfx.c
main/boot/boot_anim.c
tools/boot_anim_render_host.c
"
scene_includes="components/small3dlib/include"
scene_renders="
early|300|368x448
middle|1500|368x448
late|3000|368x448
"

# shellcheck source=./render_scene.sh
. "$SCRIPT_DIR/render_scene.sh"
render_scene_run "$@"
