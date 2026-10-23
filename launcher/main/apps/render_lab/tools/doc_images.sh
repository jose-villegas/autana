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

# The capybara's clips in the order the scene lists them, each blended into
# the next as the BLEND slider does and shown for an equal CLIP_MS share of
# the loop. Then it rewinds through them in reverse, REWIND_MS each, back to
# the first, so the loop restarts where it began, as the camera turns once
# around it. The first clip's share is split across the loop's two ends.
CLIP_DT=40
CLIP_MS=2500
CLIP_BLEND_MS=500
REWIND_MS=500
REWIND_BLEND_MS=250
clip_frame() { echo $(($1 / CLIP_DT)); }
clips=$("$W/render_lab_render" --quarter 1 --no-hud --scene capybara --frames 1 --console 0:clips \
    -o "$W/capybara-clips.bmp" 2> "$W/capybara-clips.log" | sed -n 's/^RENDER clip=\([^ ]*\) .*/\1/p')
if [ -z "$clips" ]; then
    echo "doc_images.sh: the capybara scene lists no clips; see $W/capybara-clips.log" >&2
    exit 1
fi
set -- $clips
first=$1
shift
later=$*
reversed=
for clip in $later; do
    reversed="$clip $reversed"
done
set -- $reversed
shift
rewind=$*
set --
at=$((CLIP_MS / 2))
for clip in $later; do
    set -- "$@" --console "$(clip_frame "$at"):clip $clip $CLIP_BLEND_MS"
    at=$((at + CLIP_MS))
done
for clip in $rewind $first; do
    set -- "$@" --console "$(clip_frame "$at"):clip $clip $REWIND_BLEND_MS"
    at=$((at + REWIND_MS))
done
loop_ms=$((at - REWIND_MS + CLIP_MS / 2))
run "$W/render_lab_render" --quarter 1 --no-hud --scene capybara --dt "$CLIP_DT" \
    --frames "$(clip_frame "$loop_ms")" --set render_lab.orbit_ms="$loop_ms" "$@" \
    -o "$W/capybara-motion.bmp" --video "$W/capybara-motion.avi" 2> "$W/capybara.log"
gif_from_avi "$W/capybara-motion.avi" "$OUT/render-lab-capybara.gif" "fps=25/2" "stats_mode=diff"
