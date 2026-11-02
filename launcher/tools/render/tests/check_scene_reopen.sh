#!/bin/sh
# Check that closing a run gives back its pack and framebuffer: the same scene id opened again in one
# process from another folder draws that folder's pack. REOPEN_PACKS_A, REOPEN_PACKS_B, REOPEN_OUT_B and
# REOPEN_PACKS_BAD name the first folder, the second folder, the second image and a folder whose
# pack's version the engine refuses.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
scene_name=scene_reopen_fixture
scene_provider=
scene_sources="tools/render/tests/scene_reopen_fixture.c"
scene_target=render_host
scene_renders=""
. "$SCRIPT_DIR/../render_scene.sh"
render_scene_build "$@"
"$_rs_bin" --scene room --frames 2 --dt 100 --size 184x224 -o "$scene_out_dir/reopen-a.bmp"
