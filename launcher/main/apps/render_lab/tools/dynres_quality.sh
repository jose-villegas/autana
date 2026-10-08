#!/bin/sh
#
# How close each render size of a camera path comes to the source model:
# the scene drawn at every WxH given, scored frame by frame against the
# reference render at full size, one CSV row per size and frame.
#
#   dynres_quality.sh <work-dir> <out.csv> <camera> WxH [WxH ...]
#
# Frames are every POSE_MS from one step in, matching the poses the device
# suite measures (suite_raster_scale_perf.c), so launcher/tools/r3d/
# dynres_report.py can weigh a policy's frames by the quality of their size.
# Run from the repository root with $PYTHON set to a Python with Pillow and
# numpy; the reference needs launcher/tools/r3d/requirements.txt.

set -eu

POSE_MS=2500

W=$1
CSV=$2
CAMERA=$3
shift 3
mkdir -p "$W"

sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh --build-only -o "$W" > "$W/build.log"
DEMO=launcher/demo/sponza
"$PYTHON" - "$DEMO/sponza.scene.toml" "$CAMERA" "$POSE_MS" "$W" <<'PY'
import pathlib
import sys
sys.path.insert(0, "launcher/tools")
from anim import track_host, tracks_asset
from r3d.import_settings import load_scene

scene_file, name, interval, directory = sys.argv[1:]
scene = load_scene(scene_file)
camera = next((obj.component for obj in scene.objects if obj.kind == "camera" and obj.name == name), None)
if camera is None or camera.path is None:
    raise SystemExit(f"camera {name!r} has no path")
_, period = tracks_asset.decode(tracks_asset.bake(camera.path.animation))
frames = (period + int(interval) - 1) // int(interval)
root = pathlib.Path(directory)
(root / "frames.txt").write_text(str(frames))
poses = track_host.sample(camera.path.animation, ["--every", interval, "--until", str((frames + 1) * int(interval)),
                         "--poses", camera.path.node, "368", "448", str(camera.half_fov_short_tan), str(camera.near_z)])
(root / "poses.txt").write_text(poses)
PY
FRAMES=$(cat "$W/frames.txt")
REFERENCE=$(sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "$DEMO/sponza.scene.toml" --poses "$W/poses.txt" 2> "$W/reference.log")

echo "width,height,frame,t_ms,mean_delta_e,p95_delta_e,ssim" > "$CSV"
for size in "$@"; do
    "$W/render_lab_render" --quarter 0 --no-hud --scene sponza --camera "$CAMERA" --size "$size" --frames "$FRAMES" --dt "$POSE_MS" \
        -o "$W/$size.bmp" --video "$W/$size.avi" 2> "$W/$size.log"
    "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$size-unused.png" \
        --reference-video "$W/$size.avi" "$REFERENCE" --reference-scale 1 > "$W/$size-compare.log"
    sed -n 's/^frame \([0-9]*\): mean DeltaE76 \([0-9.]*\), p95 DeltaE76 \([0-9.]*\), luma SSIM \(-\{0,1\}[0-9.]*\),.*/\1 \2 \3 \4/p' \
        "$W/$size-compare.log" | while read -r frame mean p95 ssim; do
        echo "${size%x*},${size#*x},$frame,$(((frame + 1) * POSE_MS)),$mean,$p95,$ssim"
    done >> "$CSV"
done
