#!/bin/sh
# Pin the system preferences screen through the shared host renderer.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=settings
scene_ui=1
scene_sources="
main/services/tune.c
main/app/app_registry.c
main/shell/apps/settings/app_settings.c
main/shell/apps/settings/ui/settings_screen.c
main/shell/apps/settings/ui/settings_layout.c
main/shell/apps/settings/settings_ui.c
main/shell/apps/settings/tools/settings_render_host.c
"
scene_assets="main/engine"
scene_renders="
settings|--quarter 0|368x448
settings-landscape|--quarter 1|448x368
settings-tap|--quarter 0 --tap|368x448
"
. "$SCRIPT_DIR/../../../../../tools/render/render_scene.sh"
render_scene_run "$@"
