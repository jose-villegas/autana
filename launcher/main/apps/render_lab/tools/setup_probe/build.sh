#!/bin/sh
# Builds and runs the host occlusion probe. Measurement only, never ships.
set -eu
L=$(CDPATH= cd -- "$(dirname -- "$0")/../../../../.." && pwd)
out=${TMPDIR:-/tmp}/occlusion_probe
flags="-O2 -std=gnu11 -I$L/main -I$L/test/stubs -I$L/test -I$L/components/small3dlib/include -DCONFIG_LAUNCHER_DEVELOPMENT=0"
gcc $flags -Dr3d_span_triangle=probe_span_triangle -c "$L/main/render/r3d_lit_pipeline.c" -o "$out.pipeline.o"
gcc $flags -o "$out" "$out.pipeline.o" \
    "$L/main/apps/render_lab/tools/setup_probe/occlusion_probe.c" \
    "$L/main/render/r3d_span.c" "$L/main/render/r3d_lit_frame.c" "$L/main/util/job.c" \
    "$L/main/anim/anim_track.c" "$L/main/apps/render_lab/sponza_flythrough.c" \
    "$L/main/apps/render_lab/flythrough_tracks_generated.c" \
    "$L/main/apps/render_lab/sponza_mesh_generated.c" "$L/main/apps/render_lab/sponza_lite_mesh_generated.c" -lm
"$out"
