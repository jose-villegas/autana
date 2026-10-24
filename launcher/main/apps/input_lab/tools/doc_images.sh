#!/bin/sh
#
# The input lab's images for docs/images/, made by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images.sh <out-tree> <work-dir>
#
# <out-tree> gets input/: the Motion page and its results, rendered on the host
# and turned upright, and the shake reader's trace and animation over the
# retained mixed-direction shake capture.
#
# Run from the repository root with $PYTHON set to a Python that has Pillow,
# numpy and matplotlib. Renderer output goes to logs under <work-dir>.

set -eu

. scripts/lib/run.sh
. launcher/tools/build/find_cc.sh

OUT=$1/input
W=$2
APP=launcher/main/apps/input_lab
run mkdir -p "$W" "$OUT"

run sh "$APP/tools/input_lab_render_host.sh" -o "$W" > "$W/scenes.log"
# The page is pinned to the shell's default quarter; turn it by that many quarters to read upright.
DISPLAY_H=launcher/main/display/display.h
quarter_name=$(sed -n 's/^#define DISPLAY_DEFAULT_QUARTER *\([A-Z_]*\).*/\1/p' "$DISPLAY_H")
quarter=$(sed -n "s/^#define $quarter_name *\([0-9]*\).*/\1/p" "$DISPLAY_H")
for page in motion:motion-page motion-results:motion-results; do
    run "$PYTHON" -c 'import sys; from PIL import Image
Image.open(sys.argv[1]).rotate(90 * int(sys.argv[3]), expand=True).save(sys.argv[2])' \
        "$W/${page%%:*}.bmp" "$OUT/${page#*:}.png" "$quarter"
done

cc=$(find_cc) || exit 2
samples=$W/shake-mixed.txt
run "$PYTHON" "$APP/tools/shake_fit.py" "$APP/tools/data/shake-mixed.log.gz" --samples "$samples"
run "$PYTHON" launcher/tools/input/shake_figures.py --cc "$cc" --samples "$samples" \
    --trace "$OUT/shake-trace.png" --animation "$OUT/shake-motion.gif" --step "WALK:Walking" --step "GENTLE:Gentle shaking" --step "HARD:Hard shaking" \
    > "$W/shake_figures.log"
