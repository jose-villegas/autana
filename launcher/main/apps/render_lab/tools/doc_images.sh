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
    run ffmpeg -hide_banner -loglevel error -y -t 6 -i "$W/$name-motion.avi" \
        -vf "fps=8,palettegen=stats_mode=diff" "$W/$name-palette.png"
    run ffmpeg -hide_banner -loglevel error -y -t 6 -i "$W/$name-motion.avi" -i "$W/$name-palette.png" \
        -filter_complex "[0:v]fps=8[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle" \
        -loop 0 "$OUT/render-lab-$name.gif"
done
