#!/bin/sh
#
# Render the power-on self-test screen in every orientation worth looking
# at, so the layout can be judged without a flash cycle.
#
#   ./launcher/tools/post_ui_render_host.sh [-o <dir>]
#
# Landscape is the shipping orientation, so it leads: once as the board is
# READ at that quarter, once as the panel holds it, which is the shape a
# device screenshot has and the one tools/render_diff.sh compares against.
#
# post.c is NOT among the sources; see post_ui_render_host.c's own top
# comment for what stands in for it. Everything this does beyond the
# declarations below - finding a compiler, building, checking each image
# against its declared size, converting to PNG - is tools/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=post_ui
scene_sources="
main/gfx/gfx.c
main/boot/post_ui.c
main/boot/post_layout.c
tools/post_ui_render_host.c
"
scene_renders="
landscape|--quarter 1|448x368
landscape-panel|--quarter 1 --panel|368x448
portrait|--quarter 0|368x448
landscape-fault|--quarter 1 --failures|448x368
"

# shellcheck source=./render_scene.sh
. "$SCRIPT_DIR/render_scene.sh"
render_scene_run "$@"
