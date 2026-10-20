#!/bin/sh
#
# The render lab's images for docs/images/, made by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images.sh <out-tree> <work-dir>
#
# <out-tree> holds the app's images in overview/.
#
# Run from the repository root with $PYTHON set to a Python that has Pillow and numpy;
# ffmpeg must be on PATH. Renderer output goes to logs under <work-dir>.

set -eu

. scripts/lib/run.sh
. launcher/tools/render/gif_from_avi.sh

OUT=$1/overview
W=$2
run mkdir -p "$W" "$OUT"

run sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh -o "$W" > "$W/scenes.log"

# The start of the flythrough, on the fitted full mesh and its flat-shaded bake.
for clip in sponza:sponza-fitted-full sponza-flat:sponza-flat-fitted; do
    name=${clip%%:*}
    scene=${clip#*:}
    run "$W/render_lab_render" --quarter 1 --no-hud --scene "$scene" --frames 90 --dt 100 \
        -o "$W/$name-motion.bmp" --video "$W/$name-motion.avi" 2> "$W/$name.log"
done
gif_from_avi "$W/sponza-motion.avi" "$OUT/render-lab-sponza.gif" "fps=8" "stats_mode=diff" 6
gif_from_avi "$W/sponza-flat-motion.avi" "$OUT/render-lab-sponza-flat.gif" "fps=8" "stats_mode=diff" 6

# The capybara's clips in turn - idle, walk, walk_fast, gallop - each blended
# into the next as the BLEND slider does, then back to idle so the loop
# restarts where it began, as the camera turns once around it. CLIP_MS is each
# clip's share of the 10 s loop.
CLIP_DT=40
CLIP_MS=2500
CLIP_BLEND_MS=500
clip_frame() { echo $(($1 / CLIP_DT)); }
run "$W/render_lab_render" --quarter 1 --no-hud --scene capybara --dt "$CLIP_DT" \
    --frames "$(clip_frame $((CLIP_MS * 4)))" --set render_lab.orbit_ms=$((CLIP_MS * 4)) \
    --console "$(clip_frame $((CLIP_MS / 2))):clip walk $CLIP_BLEND_MS" \
    --console "$(clip_frame $((CLIP_MS * 3 / 2))):clip walk_fast $CLIP_BLEND_MS" \
    --console "$(clip_frame $((CLIP_MS * 5 / 2))):clip gallop $CLIP_BLEND_MS" \
    --console "$(clip_frame $((CLIP_MS * 7 / 2))):clip idle $CLIP_BLEND_MS" \
    -o "$W/capybara-motion.bmp" --video "$W/capybara-motion.avi" 2> "$W/capybara.log"
gif_from_avi "$W/capybara-motion.avi" "$OUT/render-lab-capybara.gif" "fps=25/2" "stats_mode=diff"