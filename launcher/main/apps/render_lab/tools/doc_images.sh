#!/bin/sh
#
# The render lab's images for docs/images/, made by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images.sh <out-tree> <work-dir>
#
# <out-tree> holds the app's images in overview/, and the figures render pages show in render/.
#
# Run from the repository root with $PYTHON set to a Python that has Pillow and numpy.
# Renderer output goes to logs under <work-dir>.

set -eu

. scripts/lib/run.sh

OUT=$1/overview
RENDER_OUT=$1/render
W=$2
run mkdir -p "$W" "$OUT" "$RENDER_OUT"

contact_images() {
    CONTACT_FRAMES=60
    CONTACT_DT=100
    CONTACT_CROPS=3
    for shape in blob silhouette; do
        run "$W/render_lab_render" --quarter 1 --no-hud --scene capybara-sponza \
            --contact-shape "$shape" --contact-warp 0 \
            --frames "$CONTACT_FRAMES" --dt "$CONTACT_DT" \
            -o "$W/contact-$shape.bmp" > "$W/contact-$shape-console.log" 2> "$W/contact-$shape.log"
    done
    run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER_OUT/contact-silhouette.png" \
        --row "Ground contact" "$W/contact-blob.bmp" "$W/contact-silhouette.bmp" \
        --label-a BLOB --label-b SILHOUETTE --crops "$CONTACT_CROPS" > "$W/contact-silhouette-compare.log"

    for key in skinned-feet-blobs skinned-blob-warped skinned-silhouette-blobs skinned-silhouette-warped; do
        run "$W/render_lab_render" --quarter 1 --no-hud --scene "stress-$key" --frames 2 \
            -o "$W/stress-$key.bmp" 2> "$W/stress-$key.log"
        run "$PYTHON" launcher/tools/docs/figures.py png "$W/stress-$key.bmp" "$OUT/render-lab-stress-$key.png"
    done

    CONTACT_VIDEO_FRAMES=185
    CONTACT_VIDEO_FPS=5
    CONTACT_VIDEO_CROP="112 96 336 320"
    for shape in blob feet silhouette; do
        for warp in 0 1; do
            clip="$W/contact-$shape-warp-$warp"
            run "$W/render_lab_render" --quarter 1 --no-hud --scene capybara-sponza \
                --frames "$CONTACT_VIDEO_FRAMES" --dt "$CONTACT_DT" \
                --contact-shape "$shape" --contact-warp "$warp" \
                -o "$clip.bmp" --video "$clip.avi" > "$clip-console.log" 2> "$clip.log"
            run "$PYTHON" launcher/tools/docs/figures.py gif "$clip.avi" \
                "$RENDER_OUT/contact-$shape-warp-$warp.gif" --fps "$CONTACT_VIDEO_FPS" --crop-box $CONTACT_VIDEO_CROP
        done
    done

}

if [ "${3:-}" = --contacts-only ]; then
    run sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh --build-only -o "$W" > "$W/scenes.log"
    contact_images
    exit 0
fi

run sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh -o "$W" > "$W/scenes.log"

BUTTERFLY_FRAMES=50
BUTTERFLY_DT=20
run "$W/render_lab_render" --quarter 1 --no-hud --scene stress-few-butterflies --camera detail \
    --frames "$BUTTERFLY_FRAMES" --dt "$BUTTERFLY_DT" -o "$W/butterflies-motion.bmp" \
    --video "$W/butterflies-motion.avi" 2> "$W/butterflies.log"
run "$PYTHON" launcher/tools/docs/figures.py gif "$W/butterflies-motion.avi" \
    "$OUT/render-lab-butterflies.gif" --fps "1000/$BUTTERFLY_DT"

# The start of the flythrough, on the fitted full mesh.
run "$W/render_lab_render" --quarter 1 --no-hud --scene sponza --frames 90 --dt 100 \
    -o "$W/sponza-motion.bmp" --video "$W/sponza-motion.avi" 2> "$W/sponza.log"
run "$PYTHON" launcher/tools/docs/figures.py gif "$W/sponza-motion.avi" "$OUT/render-lab-sponza.gif" --fps 8 --seconds 6

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

contact_images

# The biggest herd walking for 12 s at its own pace, so each gait's feet keep
# to the ground, while the orbit makes a fifth of a turn. The camera starts at
# HERD_ZOOM hundredths of the distance that fits the whole walk (orbit_zoom):
# close enough that the copies read, far enough that most stay in view and the
# meadow fills the bottom corners.
HERD_FRAMES=150
HERD_DT=80
HERD_TURNS=5
HERD_ZOOM=65
run "$W/render_lab_render" --quarter 1 --no-hud --scene herd-8 --frames "$HERD_FRAMES" --dt "$HERD_DT" \
    --set render_lab.orbit_ms="$((HERD_FRAMES * HERD_DT * HERD_TURNS))" --set render_lab.orbit_zoom="$HERD_ZOOM" \
    -o "$W/herd-motion.bmp" \
    --video "$W/herd-motion.avi" 2> "$W/herd.log"
run "$PYTHON" launcher/tools/docs/figures.py gif "$W/herd-motion.avi" "$OUT/render-lab-herd.gif" --fps "1000/$HERD_DT"

# The capybara's whole run down Sponza's atrium, rest to rest, at 10 frames a
# second: a coarser step samples a gallop stride too few times to read.
CAPYBARA_SPONZA_DT=100
CAPYBARA_SPONZA_FRAMES=185
run "$W/render_lab_render" --quarter 1 --no-hud --scene capybara-sponza --frames "$CAPYBARA_SPONZA_FRAMES" \
    --dt "$CAPYBARA_SPONZA_DT" -o "$W/capybara-sponza-motion.bmp" --video "$W/capybara-sponza-motion.avi" \
    2> "$W/capybara-sponza.log"
run "$PYTHON" launcher/tools/docs/figures.py gif "$W/capybara-sponza-motion.avi" "$OUT/render-lab-capybara-sponza.gif" \
    --fps "1000/$CAPYBARA_SPONZA_DT"

# One still of the same run, early enough that the capybara is close and its
# contact blob shows on the floor.
CAPYBARA_SPONZA_BLOB_FRAMES=30
run "$W/render_lab_render" --quarter 1 --no-hud --scene capybara-sponza --frames "$CAPYBARA_SPONZA_BLOB_FRAMES" \
    -o "$W/capybara-sponza-blob.bmp" 2> "$W/capybara-sponza-blob.log"
run "$PYTHON" launcher/tools/docs/figures.py png "$W/capybara-sponza-blob.bmp" "$RENDER_OUT/contact-blob-atrium.png"
