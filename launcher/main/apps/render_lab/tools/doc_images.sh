#!/bin/sh
#
# The render lab's images for docs/images/, made by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images.sh <out-tree> <work-dir>
#
# <out-tree> mirrors docs/images/; these images go in its overview/ and render/.
#
# Run from the repository root with $PYTHON set to a Python that has Pillow;
# ffmpeg must be on PATH. Renderer output goes to logs under <work-dir>.

set -eu

OUT=$1/overview
RENDER=$1/render
W=$2
mkdir -p "$W" "$RENDER"

sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh -o "$W" > "$W/scenes.log"

bmp_to_png() {
    "$PYTHON" -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$1" "$2"
}

bmp_to_png "$W/gouraud-landscape.bmp" "$OUT/render-lab-cube.png"

# Fully resolved and without the HUD, which would print the fps readout over it.
"$W/render_lab_render" --quarter 1 --no-hud --scene cornell --frames 40 \
    -o "$W/cornell-clean.bmp" 2> "$W/cornell.log"
bmp_to_png "$W/cornell-clean.bmp" "$OUT/render-lab-cornell.png"

# The rotation, reversed back onto itself as a loop.
"$W/render_lab_render" --quarter 1 --no-hud --scene gouraud --frames 100 --dt 33 \
    -o "$W/cube-motion.bmp" --video "$W/cube-motion.avi" 2> "$W/cube.log"
ffmpeg -hide_banner -loglevel error -y -i "$W/cube-motion.avi" \
    -vf "fps=12,split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1:a=0,palettegen" \
    "$W/cube-palette.png"
ffmpeg -hide_banner -loglevel error -y -i "$W/cube-motion.avi" -i "$W/cube-palette.png" \
    -filter_complex "[0:v]fps=12,split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1:a=0[v];[v][1:v]paletteuse=dither=bayer" \
    -loop 0 "$OUT/render-lab-cube.gif"

# The start of the flythrough.
"$W/render_lab_render" --quarter 1 --no-hud --scene sponza --frames 90 --dt 100 \
    -o "$W/sponza-motion.bmp" --video "$W/sponza-motion.avi" 2> "$W/sponza.log"
ffmpeg -hide_banner -loglevel error -y -t 6 -i "$W/sponza-motion.avi" \
    -vf "fps=8,palettegen=stats_mode=diff" "$W/sponza-palette.png"
ffmpeg -hide_banner -loglevel error -y -t 6 -i "$W/sponza-motion.avi" -i "$W/sponza-palette.png" \
    -filter_complex "[0:v]fps=8[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle" \
    -loop 0 "$OUT/render-lab-sponza.gif"

# One looping flythrough per Sponza target and view, the same three seconds of
# it so the GIFs compare, small enough to sit side by side in a table.
sponza_gif() {
    name=$1
    shift
    "$W/render_lab_render" --quarter 1 --no-hud --frames 30 --dt 100 \
        -o "$W/$name.bmp" --video "$W/$name.avi" "$@" 2> "$W/$name.log"
    ffmpeg -hide_banner -loglevel error -y -i "$W/$name.avi" \
        -vf "fps=8,scale=240:-1:flags=lanczos,palettegen=max_colors=64:stats_mode=diff" "$W/$name-palette.png"
    ffmpeg -hide_banner -loglevel error -y -i "$W/$name.avi" -i "$W/$name-palette.png" \
        -filter_complex "[0:v]fps=8,scale=240:-1:flags=lanczos[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle" \
        -loop 0 "$RENDER/$name.gif"
}
sponza_gif sponza-full --scene sponza
sponza_gif sponza-lite --scene sponza-lite
sponza_gif sponza-flat --scene sponza-flat
sponza_gif sponza-depth --scene sponza --view depth
sponza_gif sponza-tiles --scene sponza --view tiles

# The targets' differences at the pose the GIFs end on: each pair side by side
# with the amplified difference, and the places they differ most, enlarged.
sponza_still() {
    "$W/render_lab_render" --quarter 1 --no-hud --frames 30 --dt 100 -o "$W/still-$1.bmp" --scene "$2" 2> "$W/still-$1.log"
}
sponza_still full sponza
sponza_still lite sponza-lite
sponza_still flat sponza-flat
"$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-lite.png" --crops 3 \
    --row "full | lite" "$W/still-full.bmp" "$W/still-lite.bmp" > "$W/compare-full-lite.log"
"$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-flat.png" --crops 3 \
    --row "smooth | flat" "$W/still-full.bmp" "$W/still-flat.bmp" > "$W/compare-full-flat.log"
