#!/bin/sh
# Pin the real app pages through the shared host renderer.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=input_lab
scene_ui=1
scene_sources="
main/services/tune.c
main/app/app_registry.c
main/display/display.c
main/input/tilt.c
main/input/gravity.c
main/input/imu_calib.c
main/apps/input_lab/imu_probe.c
main/apps/input_lab/imu_record.c
main/apps/input_lab/touch_probe.c
main/apps/input_lab/motion_probe.c
main/apps/input_lab/app_input_lab.c
main/apps/input_lab/tools/input_lab_render_host.c
"
scene_assets="main/engine"
scene_renders="
random|--page random --frames 6|368x448
random-tap|--page random-tap --frames 6|368x448
grid|--page grid --frames 6|368x448
bezel|--page bezel --frames 6|368x448
motion|--page motion --frames 8|368x448
motion-results|--page motion-results --frames 1500|368x448
"
. "$SCRIPT_DIR/../../../../tools/render/render_scene.sh"
render_scene_run "$@"
