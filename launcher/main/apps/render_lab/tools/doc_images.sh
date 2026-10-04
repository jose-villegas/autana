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

run() {
    if "$@"; then
        return 0
    else
        code=$?
        printf '%s: failed (exit %s):' "$0" "$code" >&2
        printf ' %s' "$@" >&2
        printf '\n' >&2
        return "$code"
    fi
}

OUT=$1/overview
RENDER=$1/render
W=$2
run mkdir -p "$W" "$RENDER"

run sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh -o "$W" > "$W/scenes.log"

bmp_to_png() {
    run "$PYTHON" -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$1" "$2"
}

bmp_to_png "$W/gouraud-landscape.bmp" "$OUT/render-lab-cube.png"

# Fully resolved and without the HUD, which would print the fps readout over it.
run "$W/render_lab_render" --quarter 1 --no-hud --scene cornell --frames 40 \
    -o "$W/cornell-clean.bmp" 2> "$W/cornell.log"
bmp_to_png "$W/cornell-clean.bmp" "$OUT/render-lab-cornell.png"

# The rotation, reversed back onto itself as a loop.
run "$W/render_lab_render" --quarter 1 --no-hud --scene gouraud --frames 100 --dt 33 \
    -o "$W/cube-motion.bmp" --video "$W/cube-motion.avi" 2> "$W/cube.log"
run ffmpeg -hide_banner -loglevel error -y -i "$W/cube-motion.avi" \
    -vf "fps=12,split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1:a=0,palettegen" \
    "$W/cube-palette.png"
run ffmpeg -hide_banner -loglevel error -y -i "$W/cube-motion.avi" -i "$W/cube-palette.png" \
    -filter_complex "[0:v]fps=12,split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1:a=0[v];[v][1:v]paletteuse=dither=bayer" \
    -loop 0 "$OUT/render-lab-cube.gif"

# The start of the flythrough, on the fitted full mesh.
run "$W/render_lab_render" --quarter 1 --no-hud --scene sponza-fitted-full --frames 90 --dt 100 \
    -o "$W/sponza-motion.bmp" --video "$W/sponza-motion.avi" 2> "$W/sponza.log"
run ffmpeg -hide_banner -loglevel error -y -t 6 -i "$W/sponza-motion.avi" \
    -vf "fps=8,palettegen=stats_mode=diff" "$W/sponza-palette.png"
run ffmpeg -hide_banner -loglevel error -y -t 6 -i "$W/sponza-motion.avi" -i "$W/sponza-palette.png" \
    -filter_complex "[0:v]fps=8[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle" \
    -loop 0 "$OUT/render-lab-sponza.gif"

# One looping flythrough per Sponza target and view, the same three seconds of
# it so the GIFs compare, small enough to sit side by side in a table.
sponza_gif() {
    name=$1
    shift
    run "$W/render_lab_render" --quarter 1 --no-hud --frames 30 --dt 100 \
        -o "$W/$name.bmp" --video "$W/$name.avi" "$@" 2> "$W/$name.log"
    run ffmpeg -hide_banner -loglevel error -y -i "$W/$name.avi" \
        -vf "fps=8,scale=240:-1:flags=lanczos,palettegen=max_colors=64:stats_mode=diff" "$W/$name-palette.png"
    run ffmpeg -hide_banner -loglevel error -y -i "$W/$name.avi" -i "$W/$name-palette.png" \
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
    run "$W/render_lab_render" --quarter 1 --no-hud --frames 30 --dt 100 -o "$W/still-$1.bmp" --scene "$2" 2> "$W/still-$1.log"
}
sponza_still full sponza
sponza_still lite sponza-lite
sponza_still flat sponza-flat
sponza_still fitted sponza-fitted
sponza_still fitted-full sponza-fitted-full
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-lite.png" --crops 3 \
    --label-a full --label-b lite --row "full | lite" "$W/still-full.bmp" "$W/still-lite.bmp" > "$W/compare-full-lite.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-flat.png" --crops 3 \
    --label-a smooth --label-b flat --row "smooth | flat" "$W/still-full.bmp" "$W/still-flat.bmp" > "$W/compare-full-flat.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-lite-fitted.png" --crops 3 \
    --label-a lite --label-b fitted --row "lite | fitted" "$W/still-lite.bmp" "$W/still-fitted.bmp" > "$W/compare-lite-fitted.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-fitted-full.png" --crops 3 \
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
run sh launcher/tools/anim/sample_tracks.sh --tracks "$M/meshes/flythrough_tracks_generated.c:flythrough" \
    --every 5000 --until 30000 --poses camera 184 224 0.62 6 > "$W/fidelity-poses.txt"
# render_compare.sh keeps the traced frames in r3d/.cache/reference by a hash of
# their inputs, so only a change to the scene, tracer or poses traces again.
REFERENCE=$(run sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "$M/meshes/sponza.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/fidelity-reference.log")
run "$W/render_lab_render" --quarter 0 --no-hud --scene sponza-flat --frames 5 --dt 5000 \
    -o "$W/fidelity-flat.bmp" --video "$W/fidelity-flat.avi" 2> "$W/fidelity-flat.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/fidelity-unused.png" \
    --reference-video "$W/fidelity-flat.avi" "$REFERENCE" --reference-scale 2 \
    --reference-sheet "$RENDER/bake-fidelity-sheet.png" --sheet-frames 2,4 --label-a "flat bake" > "$W/fidelity-compare.log"

# The smooth bake with indirect light against the reference with the same
# light, and bakes of the same import that differ only in what the scene says
# about indirect light, each rendered at the same five poses. Each bake's
# directory holds copies of the import and the scene, packed in place of the
# committed mesh, so nothing committed changes.
. scripts/lib/python.sh
R3D_PYTHON=$(run find_r3d_python "$PWD")
run "$W/render_lab_render" --quarter 0 --no-hud --scene sponza --frames 5 --dt 5000 \
    -o "$W/indirect-smooth.bmp" --video "$W/indirect-smooth.avi" 2> "$W/indirect-smooth.log"
# variant_bake NAME BOUNCES SCENE-TABLE: bounces is `keep`, or `none` to take
# the scene bake's indirect cache out; the table goes before the first object.
variant_bake() {
    run mkdir -p "$W/indirect-$1"
    run cp "$M/meshes/sponza.import.toml" "$W/indirect-$1/"
    run awk -v table="$3" -v direct="$2" '/^\[\[objects\]\]/ && !done { if (table != "") print table "\n"; done = 1 }
        direct == "none" && /^indirect = \{/ { next } { print }' \
        "$M/meshes/sponza.scene.toml" > "$W/indirect-$1/sponza.scene.toml"
    run "$R3D_PYTHON" launcher/tools/r3d/mesh_import.py "$W/indirect-$1/sponza.scene.toml" --mesh sponza.atrium > "$W/indirect-$1/bake.log" 2>&1
    run "$R3D_PYTHON" launcher/tools/r3d/build_pack.py -o "$W/indirect-$1/assets.bin" --replace "sponza.atrium=$W/indirect-$1/sponza.atrium.mesh" \
        > "$W/indirect-$1/pack.log"
    run env AUTANA_ASSET_PACK="$W/indirect-$1/assets.bin" "$W/render_lab_render" --quarter 0 --no-hud --scene sponza --frames 5 --dt 5000 \
        -o "$W/indirect-$1/frame.bmp" --video "$W/indirect-$1.avi" 2> "$W/indirect-$1/render.log"
}
variant_bake direct none ''
variant_bake intensity-2 keep '[indirect]\nintensity = 2.0'
variant_bake intensity-3 keep '[indirect]\nintensity = 3.0'
variant_bake boost-2 keep '[indirect]\nalbedo_boost = 2.0'

# Reference, direct-only bake and two-bounce bake at two poses, with each
# bake's dE heatmap against the reference, then the places the two bakes differ
# most with the reference above them.
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/indirect-compare.png" --crops 4 \
    --reference-bakes "$REFERENCE" --reference-scale 2 --sheet-frames 2,4 \
    --bake "direct light only" "$W/indirect-direct.avi" --bake "two bounces" "$W/indirect-smooth.avi" > "$W/indirect-compare.log"
[ -f "$W/indirect-compare.crops.png" ] || { echo "doc_images.sh: indirect light has no crops, the bakes do not differ." >&2; exit 1; }
run cp "$W/indirect-compare.png" "$RENDER/bake-indirect-compare.png"
run cp "$W/indirect-compare.crops.png" "$RENDER/bake-indirect-crops.png"

# The look controls: the physical bake, indirect intensity 2 and 3, and an
# albedo boost of 2, last pose, beside the physical reference and, under it,
# each look's own reference: the scene's [indirect] table reaches the reference
# too, so the error against it is the bake's alone.
look_reference() {
    run sh launcher/tools/render/render_compare.sh --reference-frames \
        --reference "$W/indirect-$1/sponza.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/indirect-$1/reference.log"
}
INTENSITY_2_REFERENCE=$(look_reference intensity-2)
INTENSITY_3_REFERENCE=$(look_reference intensity-3)
BOOST_2_REFERENCE=$(look_reference boost-2)
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/bake-indirect-look.png" \
    --reference-bakes "$REFERENCE" --reference-scale 2 --sheet-frames 4 \
    --bake "intensity 1" "$W/indirect-smooth.avi" --bake "intensity 2" "$W/indirect-intensity-2.avi" \
    --bake "intensity 3" "$W/indirect-intensity-3.avi" --bake "albedo boost 2" "$W/indirect-boost-2.avi" \
    --bake-reference "intensity 1" "$REFERENCE" --bake-reference "intensity 2" "$INTENSITY_2_REFERENCE" \
    --bake-reference "intensity 3" "$INTENSITY_3_REFERENCE" --bake-reference "albedo boost 2" "$BOOST_2_REFERENCE" \
    > "$W/indirect-look.log"

# Each fitted target against the same reference: its heatmap sheet at the
# same two poses, and its last frame beside the reference, enlarged where they
# differ most.
#   against_reference <scene> <heatmap image> <reference image> <label> [--sheet] [--crops]
run "$PYTHON" -c 'import pathlib, sys; from PIL import Image
frame = sorted(pathlib.Path(sys.argv[1]).glob("*.png"))[4]
picture = Image.open(frame).convert("RGB")
picture.resize((picture.width * 2, picture.height * 2), Image.Resampling.NEAREST).save(sys.argv[2])' \
    "$REFERENCE" "$W/fidelity-reference-4.png"
against_reference() {
    scene=$1 heat=$2 reference=$3 label=$4
    shift 4
    sheet=no crops=no
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --sheet) sheet=yes ;;
            --crops) crops=yes ;;
            *) echo "doc_images.sh: against_reference unknown option $1" >&2; exit 2 ;;
        esac
        shift
    done
    run "$W/render_lab_render" --quarter 0 --no-hud --scene "$scene" --frames 5 --dt 5000 \
        -o "$W/fidelity-$scene.bmp" --video "$W/fidelity-$scene.avi" 2> "$W/fidelity-$scene.log"
    run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$scene-unused.png" \
        --reference-video "$W/fidelity-$scene.avi" "$REFERENCE" --reference-scale 2 \
        --reference-sheet "$RENDER/$heat.png" --sheet-frames 2,4 --label-a "$label" > "$W/$scene-compare.log"
    if [ "$crops" = yes ]; then
        run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$reference.png" --crops 3 \
            --label-a "$label" --label-b reference --row "$label | reference" "$W/fidelity-$scene.bmp" "$W/fidelity-reference-4.png" > "$W/$scene-reference.log"
        cp "$W/$reference.crops.png" "$RENDER/" || {
            echo "doc_images.sh: $scene matches the reference, no crops." >&2
            exit 1
        }
    fi
    [ "$sheet" = no ] || cp "$W/$reference.png" "$RENDER/"
}
against_reference sponza-fitted appearance-chosen-heat appearance-chosen-reference fitted --crops
against_reference sponza-fitted-full appearance-fitted-full-heat appearance-fitted-full-reference "fitted full" --sheet --crops
against_reference sponza-lite appearance-lite-reference appearance-lite-reference-row simplifier
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/appearance-lite-fitted-reference.crops.png" --crops 3 \
    --label-a simplifier --label-b fitted --reference-crops "$W/fidelity-reference-4.png" \
    "$W/fidelity-sponza-lite.bmp" "$W/fidelity-sponza-fitted.bmp" > "$W/appearance-lite-fitted-reference.log"
