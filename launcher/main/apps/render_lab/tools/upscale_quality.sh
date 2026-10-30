#!/bin/sh
#
# Nearest against surface upscaling: each scene's camera path drawn at the
# half-width steps under the half picture with each filter, scored against the
# reference at full size, and the frames of one for the docs' sheet.
#
#   upscale_quality.sh <work-dir> <out.csv> <frames-dir> <camera> \
#       <scene-file>:<key> [<scene-file>:<key> ...]
#
# The CSV is dynres_quality.sh's columns after scene and filter. The frames
# are the first scene's at the middle size where the surface filter lowers
# the edge error most: <frames-dir>/<scene>-<size>-<frame>-reference.png,
# -nearest.png and -surface.png, which render_doc_images.sh lays out.
# Run from the repository root with $PYTHON set as dynres_quality.sh needs.

set -eu

SIZES="184x224 184x149 184x112"
FIGURE_SIZE=184x149
FILTERS="nearest surface"

W=$1
CSV=$2
FRAMES=$3
CAMERA=$4
shift 4
mkdir -p "$W" "$FRAMES"

echo "scene,filter,width,height,frame,t_ms,mean_delta_e,p95_delta_e,ssim,edge_delta_e,interior_delta_e" > "$CSV"
for scene in "$@"; do
    file=${scene%:*}
    key=${scene##*:}
    tune=0
    for filter in $FILTERS; do
        # shellcheck disable=SC2086 # SIZES is one argument per size
        sh launcher/main/apps/render_lab/tools/dynres_quality.sh --scene-file "$file" --scene "$key" \
            --args "--tune render_lab.upscale=$tune" "$W/$key-$filter" "$W/$key-$filter.csv" "$CAMERA" $SIZES
        tail -n +2 "$W/$key-$filter.csv" | sed "s/^/$key,$filter,/" >> "$CSV"
        tune=$((tune + 1))
    done
done

first=${1##*:}
REFERENCE=$(sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "${1%:*}" --poses "$W/$first-nearest/poses.txt" 2> /dev/null)
"$PYTHON" - "$W/$first-nearest/$FIGURE_SIZE.avi" "$W/$first-surface/$FIGURE_SIZE.avi" "$REFERENCE" \
    "$FRAMES/$first-$FIGURE_SIZE" <<'PY'
import pathlib
import sys
sys.path.insert(0, "launcher/tools/render")
from PIL import Image
import render_compare

nearest_avi, surface_avi, reference_dir, prefix = sys.argv[1:]
references = sorted(pathlib.Path(reference_dir).glob("*.png"))
_, nearest = render_compare.read_video(nearest_avi)
_, surface = render_compare.read_video(surface_avi)
best = None
for index, (a, b) in enumerate(zip(nearest, surface)):
    a, b = Image.fromarray(a), Image.fromarray(b)
    reference = Image.open(references[index]).resize(a.size, Image.Resampling.NEAREST)
    gain = (render_compare.reference_measure(a, reference).edge_delta_e
            - render_compare.reference_measure(b, reference).edge_delta_e)
    if best is None or gain > best[0]:
        best = (gain, index, a, b, reference)
_, index, a, b, reference = best
for name, picture in (("reference", reference), ("nearest", a), ("surface", b)):
    picture.save(f"{prefix}-{index}-{name}.png", optimize=True)
PY
