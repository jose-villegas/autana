#!/bin/sh
#
# The render lab's images for docs/images/, made by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images.sh <out-tree> <work-dir>
#
# <out-tree> holds the app's images in overview/.
#
# Run from the repository root with $PYTHON set to a Python that has Pillow and numpy.
# Renderer output goes to logs under <work-dir>.

set -eu

. scripts/lib/run.sh

OUT=$1/overview
W=$2
run mkdir -p "$W" "$OUT"

run sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh -o "$W" > "$W/scenes.log"

# The start of the flythrough, on the fitted full mesh.
run "$W/render_lab_render" --quarter 1 --no-hud --scene sponza-fitted-full --frames 90 --dt 100 \
    -o "$W/sponza-motion.bmp" --video "$W/sponza-motion.avi" 2> "$W/sponza.log"
run "$PYTHON" launcher/tools/docs/figures.py gif "$W/sponza-motion.avi" "$OUT/render-lab-sponza.gif" --fps 8 --seconds 6

# The capybara's clips in the order the scene lists them, each blended into
# the next as the BLEND slider does and shown for an equal CLIP_MS share of
# the loop, then back to the first so the loop restarts where it began, as
# the camera turns once around it. The first clip's share is split across
# the loop's two ends. The return to the first clip blends over RETURN_BLEND_MS,
# slower than the others; the loop ends once it has finished.
CLIP_DT=40
CLIP_MS=2500
CLIP_BLEND_MS=500
RETURN_BLEND_MS=1500
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
set --
at=$((CLIP_MS / 2))
for clip in $later; do
    set -- "$@" --console "$(clip_frame "$at"):clip $clip $CLIP_BLEND_MS"
    at=$((at + CLIP_MS))
done
set -- "$@" --console "$(clip_frame "$at"):clip $first $RETURN_BLEND_MS"
return_ms=$((CLIP_MS / 2))
if [ "$RETURN_BLEND_MS" -gt "$return_ms" ]; then
    return_ms=$RETURN_BLEND_MS
fi
loop_ms=$((at + return_ms))
run "$W/render_lab_render" --quarter 1 --no-hud --scene capybara --dt "$CLIP_DT" \
    --frames "$(clip_frame "$loop_ms")" --set render_lab.orbit_ms="$loop_ms" "$@" \
    -o "$W/capybara-motion.bmp" --video "$W/capybara-motion.avi" > "$W/capybara-console.log" 2> "$W/capybara.log"
if grep -q "^RENDER_ERR" "$W/capybara-console.log"; then
    echo "doc_images.sh: the renderer refused a clip command; see $W/capybara-console.log" >&2
    exit 1
fi
run "$PYTHON" launcher/tools/docs/figures.py gif "$W/capybara-motion.avi" "$OUT/render-lab-capybara.gif" --fps 25/2

# The water scene's swing (`water swing`), landscape as the other clips:
# rocked hard and then calmer, leaned back so the surface shows, its small
# triangles filled with the SMALL FILL menu's dithered preset. The scene's
# reply gives the swing's length.
WATER_DT=16
WATER_SMALL_FILL="FAST - DITHER"
swing_ms=$("$W/render_lab_render" --quarter 1 --no-hud --scene water --frames 1 --console "0:water swing" \
    -o "$W/water-swing-length.bmp" 2> "$W/water-swing-length.log" | sed -n 's/^RENDER water .*script_ms=\([0-9]*\).*/\1/p')
if [ -z "$swing_ms" ]; then
    echo "doc_images.sh: the water scene gave no swing length; see $W/water-swing-length.log" >&2
    exit 1
fi
run "$W/render_lab_render" --quarter 1 --no-hud --scene water --small-fill "$WATER_SMALL_FILL" \
    --console "0:water swing" --dt "$WATER_DT" --frames "$((swing_ms / WATER_DT))" -o "$W/water-swing.bmp" --video "$W/water-swing.avi" 2> "$W/water.log"
run "$PYTHON" launcher/tools/docs/figures.py gif "$W/water-swing.avi" "$OUT/render-lab-water.gif" --fps 15

# Each stress preset as its swing starts, in the order stress_preset.h lists them.
for key in $(sed -n 's/^ *X([A-Z_]*, "\([^"]*\)".*/\1/p' launcher/main/apps/render_lab/stress/stress_preset.h); do
    run "$W/render_lab_render" --quarter 1 --no-hud --scene "stress-$key" --frames 2 -o "$W/stress-$key.bmp" \
        2> "$W/stress-$key.log"
    run "$PYTHON" launcher/tools/docs/figures.py png "$W/stress-$key.bmp" "$OUT/render-lab-stress-$key.png"
done

# The biggest herd walking for 12 s at its own pace, so each gait's feet keep
# to the ground, while the orbit makes a fifth of a turn.
HERD_FRAMES=150
HERD_DT=80
HERD_TURNS=5
run "$W/render_lab_render" --quarter 1 --no-hud --scene herd-8 --frames "$HERD_FRAMES" --dt "$HERD_DT" \
    --set render_lab.orbit_ms="$((HERD_FRAMES * HERD_DT * HERD_TURNS))" -o "$W/herd-motion.bmp" \
    --video "$W/herd-motion.avi" 2> "$W/herd.log"
run "$PYTHON" launcher/tools/docs/figures.py gif "$W/herd-motion.avi" "$OUT/render-lab-herd.gif" --fps "1000/$HERD_DT"

# The capybara's whole run down Sponza's atrium, rest to rest.
CAPYBARA_SPONZA_FRAMES=120
CAPYBARA_SPONZA_DT=150
run "$W/render_lab_render" --quarter 1 --no-hud --scene capybara-sponza --frames "$CAPYBARA_SPONZA_FRAMES"     --dt "$CAPYBARA_SPONZA_DT" -o "$W/capybara-sponza-motion.bmp" --video "$W/capybara-sponza-motion.avi"     2> "$W/capybara-sponza.log"
run "$PYTHON" launcher/tools/docs/figures.py gif "$W/capybara-sponza-motion.avi" "$OUT/render-lab-capybara-sponza.gif"     --fps "1000/$CAPYBARA_SPONZA_DT"
