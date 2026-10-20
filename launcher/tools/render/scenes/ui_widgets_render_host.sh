#!/bin/sh
#
# The shell's UI toolkit drawn on a host, the images docs/UI-Toolkit.md
# shows, see ui_widgets_render_host.c and docs/tools/Render-Harness.md.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=ui_widgets
scene_ui=1
scene_sources="
main/services/tune.c
tools/render/scenes/ui_widgets_render_host.c
"
scene_assets="main/engine"
scene_renders="
widgets|--quarter 0 --view widgets|368x448
widgets-landscape|--quarter 1 --view widgets|448x368
dropdown-open|--quarter 0 --view dropdown-open|368x448
dropdown-open-landscape|--quarter 1 --view dropdown-open|448x368
controls|--quarter 0 --view controls|368x448
controls-landscape|--quarter 1 --view controls|448x368
settings|--quarter 0 --view settings|368x448
settings-landscape|--quarter 1 --view settings|448x368
widgets-no-pack|--quarter 0 --view widgets|368x448|nopacks
settings-no-pack|--quarter 0 --view settings|368x448|nopacks
"

. "$SCRIPT_DIR/../render_scene.sh"
render_scene_run "$@"
