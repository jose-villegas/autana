#!/bin/sh
#
# Render this app's INTEGER scenes with the fps/title HUD hidden, and pin
# them - render_lab_render_host.sh's own renders stay unpinned because they
# carry that HUD (a double formatted with "%.1f" defeats a pin), and this
# app also has the HUD to check by eye, which is what that script is for.
#
#   ./launcher/main/apps/render_lab/tools/render_lab_pinned_render_host.sh [-o <dir>]
#
# Both Cornell box scenes are float throughout and are left out here:
# Render-Harness.md only pins where the pixels are integer-exact. Their
# sources still build, since app_render_lab.c links every scene it lists.
#
# Everything beyond the declarations below is
# launcher/tools/render_scene.sh, shared with render_lab_render_host.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=render_lab_pinned
. "$SCRIPT_DIR/render_lab_render_sources.sh"

scene_renders="
gouraud-landscape|--quarter 1 --no-hud --scene gouraud|448x368
gouraud-landscape-panel|--quarter 1 --panel --no-hud --scene gouraud|368x448
gouraud-portrait|--quarter 0 --no-hud --scene gouraud|368x448
plane-landscape|--quarter 1 --no-hud --scene plane|448x368
plane-portrait|--quarter 0 --no-hud --scene plane|368x448
sphere-landscape|--quarter 1 --no-hud --scene sphere|448x368
sphere-portrait|--quarter 0 --no-hud --scene sphere|368x448
wirecube-landscape|--quarter 1 --no-hud --scene cube|448x368
capsule-landscape|--quarter 1 --no-hud --scene capsule|448x368
"

# shellcheck source=../../../../tools/render_scene.sh
. "$SCRIPT_DIR/../../../../tools/render_scene.sh"
render_scene_run "$@"
