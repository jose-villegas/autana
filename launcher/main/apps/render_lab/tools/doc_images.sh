#!/bin/sh
#
# The render lab's images for docs/images/, made by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images.sh <out-tree> <work-dir>
#
# <out-tree> mirrors docs/images/; these images go in its overview/ and render/.
#
# Run from the repository root with $PYTHON set to a Python that has Pillow and numpy
# (the fidelity sheet also needs launcher/tools/r3d/requirements.txt, found by find_r3d_python);
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
sponza_gif sponza-fitted --scene sponza-fitted
sponza_gif sponza-fitted-full --scene sponza-fitted-full
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
sponza_still fitted sponza-fitted
sponza_still fitted-full sponza-fitted-full
"$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-lite.png" --crops 3 \
    --label-a full --label-b lite --row "full | lite" "$W/still-full.bmp" "$W/still-lite.bmp" > "$W/compare-full-lite.log"
"$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-flat.png" --crops 3 \
    --label-a smooth --label-b flat --row "smooth | flat" "$W/still-full.bmp" "$W/still-flat.bmp" > "$W/compare-full-flat.log"
"$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-lite-fitted.png" --crops 3 \
    --label-a lite --label-b fitted --row "lite | fitted" "$W/still-lite.bmp" "$W/still-fitted.bmp" > "$W/compare-lite-fitted.log"
"$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-fitted-full.png" --crops 3 \
    --label-a full --label-b "fitted full" --row "full | fitted full" "$W/still-full.bmp" "$W/still-fitted-full.bmp" > "$W/compare-full-fitted-full.log"
# render_compare.py writes no crops where the two renders do not differ; fail
# here rather than leave the pages linking a missing file.
for crops in compare-full-lite compare-full-flat compare-lite-fitted compare-full-fitted-full; do
    [ -f "$RENDER/$crops.crops.png" ] || { echo "doc_images.sh: $crops has no crops, the renders do not differ." >&2; exit 1; }
done

# The fidelity sheet: two poses of the committed flat bake against the source
# model lit per pixel. Poses at 0 to 25000 ms every 5000 give render frames 0 to 4,
# and the sheet shows frames 2 and 4. The source model is fetched once, SHA-256
# checked, into launcher/tools/r3d/.cache.
M=launcher/main/apps/render_lab
sh launcher/tools/anim/sample_tracks.sh --tracks "$M/flythrough_tracks_generated.c:flythrough" \
    --every 5000 --until 30000 --poses camera 184 224 0.62 6 > "$W/fidelity-poses.txt"
# render_compare.sh keeps the traced frames in r3d/.cache/reference by a hash of
# their inputs, so only a change to the scene, tracer or poses traces again.
REFERENCE=$(sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "$M/meshes/sponza.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/fidelity-reference.log")
"$W/render_lab_render" --quarter 0 --no-hud --scene sponza-flat --frames 5 --dt 5000 \
    -o "$W/fidelity-flat.bmp" --video "$W/fidelity-flat.avi" 2> "$W/fidelity-flat.log"
"$PYTHON" launcher/tools/render/render_compare.py --out "$W/fidelity-unused.png" \
    --reference-video "$W/fidelity-flat.avi" "$REFERENCE" --reference-scale 2 \
    --reference-sheet "$RENDER/bake-fidelity-sheet.png" --sheet-frames 2,4 --label-a "flat bake" > "$W/fidelity-compare.log"

# Each fitted target against the same reference: its heatmap sheet at the
# same two poses, and its last frame beside the reference, enlarged where they
# differ most.
#   against_reference <scene> <heatmap image> <reference image> <label> <sheet too: yes|no>
"$PYTHON" -c 'import pathlib, sys; from PIL import Image
frame = sorted(pathlib.Path(sys.argv[1]).glob("*.png"))[4]
picture = Image.open(frame).convert("RGB")
picture.resize((picture.width * 2, picture.height * 2), Image.Resampling.NEAREST).save(sys.argv[2])' \
    "$REFERENCE" "$W/fidelity-reference-4.png"
against_reference() {
    "$W/render_lab_render" --quarter 0 --no-hud --scene "$1" --frames 5 --dt 5000 \
        -o "$W/fidelity-$1.bmp" --video "$W/fidelity-$1.avi" 2> "$W/fidelity-$1.log"
    "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$1-unused.png" \
        --reference-video "$W/fidelity-$1.avi" "$REFERENCE" --reference-scale 2 \
        --reference-sheet "$RENDER/$2.png" --sheet-frames 2,4 --label-a "$4" > "$W/$1-compare.log"
    "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$3.png" --crops 3 \
        --label-a "$4" --label-b reference --row "$4 | reference" "$W/fidelity-$1.bmp" "$W/fidelity-reference-4.png" > "$W/$1-reference.log"
    cp "$W/$3.crops.png" "$RENDER/" || {
        echo "doc_images.sh: $1 matches the reference, no crops." >&2
        exit 1
    }
    [ "$5" = no ] || cp "$W/$3.png" "$RENDER/"
}
against_reference sponza-fitted appearance-chosen-heat appearance-chosen-reference fitted no
against_reference sponza-fitted-full appearance-fitted-full-heat appearance-fitted-full-reference "fitted full" yes
