#!/usr/bin/env sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=sand_sim
scene_sources="
main/gfx/gfx.c
main/util/tune.c
main/util/job.c
main/apps/sand/sand.c
main/apps/sand/sand_chunk_sched.c
main/apps/sand/sand_impulse.c
main/apps/sand/sand_reactions.c
main/apps/sand/sand_plants.c
main/apps/sand/sand_gas.c
main/apps/sand/sand_liquid.c
main/apps/sand/material.c
main/apps/sand/material_palette.c
main/apps/sand/tools/sand_sim_render_host.c
"
scene_renders="
simulation-portrait|--quarter 0|368x448
"

. "$SCRIPT_DIR/../../../../tools/render/render_scene.sh"
render_scene_run "$@"
