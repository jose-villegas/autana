#!/bin/sh
# Pin the developer-toggles page through the shared host renderer.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=diagnostics
scene_ui=1
scene_sources="
main/services/tune.c
main/app/app_registry.c
main/shell/apps/diagnostics/app_diagnostics.c
main/shell/apps/diagnostics/ui/toggles_screen.c
main/shell/apps/diagnostics/tools/diagnostics_render_host.c
"
scene_assets="main/engine"
scene_renders="
toggles|--quarter 0|368x448
toggles-landscape|--quarter 1|448x368
toggles-tap|--quarter 0 --tap|368x448
"
. "$SCRIPT_DIR/../../../../../tools/render/render_scene.sh"
render_scene_run "$@"
