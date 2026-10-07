#!/bin/sh
#
# How close each render size of the flythrough comes to the source model:
# the scene drawn at every WxH given, scored frame by frame against the
# reference render at full size, one CSV row per size and frame.
#
#   dynres_quality.sh <work-dir> <out.csv> WxH [WxH ...]
#
# Frames are every POSE_MS from one step in, matching the poses the device
# suite measures (suite_raster_scale_perf.c), so launcher/tools/r3d/
# dynres_report.py can weigh a policy's frames by the quality of their size.
# Run from the repository root with $PYTHON set to a Python with Pillow and
# numpy; the reference needs launcher/tools/r3d/requirements.txt.

set -eu

POSE_MS=2500
FRAMES=14

W=$1
CSV=$2
shift 2
mkdir -p "$W"

sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh --build-only -o "$W" > "$W/build.log"
M=launcher/main/apps/render_lab
"$PYTHON" launcher/tools/anim/track_host.py "$M/assets/flythrough.anim.toml" \
    --every "$POSE_MS" --until $(((FRAMES + 1) * POSE_MS)) --poses camera 368 448 0.62 6 > "$W/poses.txt"
REFERENCE=$(sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "$M/meshes/sponza.scene.toml" --poses "$W/poses.txt" 2> "$W/reference.log")

echo "width,height,frame,t_ms,mean_delta_e,p95_delta_e,ssim" > "$CSV"
for size in "$@"; do
    "$W/render_lab_render" --quarter 0 --no-hud --scene sponza --size "$size" --frames "$FRAMES" --dt "$POSE_MS" \
        -o "$W/$size.bmp" --video "$W/$size.avi" 2> "$W/$size.log"
    "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$size-unused.png" \
        --reference-video "$W/$size.avi" "$REFERENCE" --reference-scale 1 > "$W/$size-compare.log"
    sed -n 's/^frame \([0-9]*\): mean DeltaE76 \([0-9.]*\), p95 DeltaE76 \([0-9.]*\), luma SSIM \(-\{0,1\}[0-9.]*\),.*/\1 \2 \3 \4/p' \
        "$W/$size-compare.log" | while read -r frame mean p95 ssim; do
        echo "${size%x*},${size#*x},$frame,$(((frame + 1) * POSE_MS)),$mean,$p95,$ssim"
    done >> "$CSV"
done
