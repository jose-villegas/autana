#!/bin/sh
#
# The render lab's images for docs/images/, made by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images.sh <out-tree> <work-dir>
#
# <out-tree> holds images in overview/ and render/, and measured blocks in tables/.
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
TABLES=$1/tables
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
# directory holds copies of the import and the scene and a link to the mesh
# source they name, packed in place of the committed mesh, so nothing committed
# changes.
. scripts/lib/python.sh
R3D_PYTHON=$(run find_r3d_python "$PWD")
run "$W/render_lab_render" --quarter 0 --no-hud --scene sponza --frames 5 --dt 5000 \
    -o "$W/indirect-smooth.bmp" --video "$W/indirect-smooth.avi" 2> "$W/indirect-smooth.log"
# bake_and_render DIR MESH SCENE: link the mesh source into DIR, bake the scene file in DIR,
# pack the mesh in place of the committed one and render the five poses to DIR.avi.
bake_and_render() {
    dir=$1 mesh=$2 scene=$3
    run ln -sfn "$PWD/$M/meshes/sponza" "$dir/sponza"
    run "$R3D_PYTHON" launcher/tools/r3d/mesh_import.py "$dir/sponza.scene.toml" --mesh "$mesh" > "$dir/bake.log" 2>&1
    run "$R3D_PYTHON" launcher/tools/r3d/build_pack.py -o "$dir/assets.bin" --replace "$mesh=$dir/$mesh.mesh" > "$dir/pack.log"
    run env AUTANA_ASSET_PACK="$dir/assets.bin" "$W/render_lab_render" --quarter 0 --no-hud --scene "$scene" --frames 5 --dt 5000 \
        -o "$dir/frame.bmp" --video "$dir.avi" 2> "$dir/render.log"
}
# variant_bake NAME BOUNCES SCENE-TABLE: bounces is `keep`, or `none` to take
# the scene bake's indirect cache out; the table goes before the first object.
variant_bake() {
    run mkdir -p "$W/indirect-$1"
    run cp "$M/meshes/sponza.import.toml" "$W/indirect-$1/"
    run awk -v table="$3" -v direct="$2" '/^\[\[objects\]\]/ && !done { if (table != "") print table "\n"; done = 1 }
        direct == "none" && /^indirect = \{/ { next } { print }' \
        "$M/meshes/sponza.scene.toml" > "$W/indirect-$1/sponza.scene.toml"
    bake_and_render "$W/indirect-$1" "${4:-sponza.atrium}" "${5:-sponza}"
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

# The scene's ambient light is faint, so the occlusion has little to scale; both
# bakes raise it. ao_bake NAME AO-LINE: the line goes after `[bake].indirect`.
ao_bake() {
    run mkdir -p "$W/ao-$1"
    run cp "$M/meshes/sponza.import.toml" "$W/ao-$1/"
    run awk -v ao="$2" '/^\[/ { ambient = ($0 == "[ambient]") } ambient && /^intensity = / { print "intensity = 0.25"; next }
        { print } /^indirect = \{/ && ao != "" { print ao }' \
        "$M/meshes/sponza.scene.toml" > "$W/ao-$1/sponza.scene.toml"
    bake_and_render "$W/ao-$1" sponza.atrium sponza
}
ao_bake flat ''
ao_bake occluded 'ao = { distance = 80.0, rays = 32 }'
AO_REFERENCE=$(run sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "$W/ao-occluded/sponza.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/ao-occluded/reference.log")
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/ao-compare.png" --crops 4 \
    --reference-bakes "$AO_REFERENCE" --reference-scale 2 --sheet-frames 2,4 \
    --bake "no occlusion" "$W/ao-flat.avi" --bake "occlusion" "$W/ao-occluded.avi" > "$W/ao-compare.log"
[ -f "$W/ao-compare.crops.png" ] || { echo "doc_images.sh: occlusion has no crops, the bakes do not differ." >&2; exit 1; }
run cp "$W/ao-compare.png" "$RENDER/bake-ao-compare.png"
run cp "$W/ao-compare.crops.png" "$RENDER/bake-ao-crops.png"

# The occlusion factor beside the reference frame at the sheet's two poses.
run "$R3D_PYTHON" launcher/tools/r3d/reference_render.py "$W/ao-occluded/sponza.scene.toml" --poses "$W/fidelity-poses.txt" \
    --skip 1 --samples 4 --occlusion --out "$W/ao-map" > "$W/ao-map.log" 2>&1
run "$PYTHON" -c 'import sys; from PIL import Image
rows = []
for frame in (2, 4):
    pair = [Image.open(f"{sys.argv[1]}/{frame:04d}{suffix}.png").convert("RGB") for suffix in (".occlusion", "")]
    rows.append([picture.resize((picture.width * 2, picture.height * 2), Image.Resampling.NEAREST) for picture in pair])
width, height = rows[0][0].size
sheet = Image.new("RGB", (width * 2, height * len(rows)))
for y, row in enumerate(rows):
    for x, picture in enumerate(row):
        sheet.paste(picture, (x * width, y * height))
sheet.save(sys.argv[2])' "$W/ao-map" "$RENDER/bake-ao-map.png"

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

# Direct-light counterparts at the same poses; the committed indirect bakes
# and source reference are shared with the image measurements above.
tables_start=$(date +%s)
variant_bake direct-lite none '' sponza.atrium_lite sponza-lite
variant_bake direct-flat none '' sponza.atrium_flat sponza-flat
for kind in lite flat; do
    "$PYTHON" launcher/tools/render/render_compare.py --out "$W/direct-$kind-unused.png" \
        --reference-video "$W/indirect-direct-$kind.avi" "$REFERENCE" --reference-scale 2 > "$W/direct-$kind-compare.log"
done

sweep_start=$(date +%s)
"$R3D_PYTHON" launcher/tools/r3d/bake_fidelity.py "$M/meshes/sponza.scene.toml" --mesh atrium_flat \
    --host "$W/render_lab_render" \
    --render-args "--quarter 0 --no-hud --scene sponza-flat --frames 5 --dt 5000" \
    --reference "$REFERENCE" --work "$W/sampling" \
    --variant declared= --variant fixed1=samples=fixed:1 --variant fixed2=samples=fixed:2 \
    --variant fixed4=samples=fixed:4 --variant fixed8=samples=fixed:8 --variant fixed16=samples=fixed:16 \
    --variant fixed32=samples=fixed:32 --variant fixed64=samples=fixed:64 \
    --variant min2=samples=auto:2:16:median --variant min4=samples=auto:4:16:median \
    --variant max4=samples=auto:1:4:median --variant max8=samples=auto:1:8:median \
    --variant max32=samples=auto:1:32:median --variant area0.25=samples=auto:1:16:median*0.25 \
    --variant area0.5=samples=auto:1:16:median*0.5 --variant area2=samples=auto:1:16:median*2 \
    --variant sky16=sky=16 --variant sky32=sky=32 --variant sky64=sky=64 \
    --variant sky256=sky=256 --variant sky512=sky=512 --variant centroid=place=centroid \
    --variant sun-centre=sun=centre --variant fixed4-sun-centre=samples=fixed:4,sun=centre \
    > "$W/sampling.log" 2>&1
sweep_seconds=$(($(date +%s) - sweep_start))
echo "CPU flat sampling sweep: $sweep_seconds seconds"
echo "$sweep_seconds" > "$W/sweep-seconds.txt"
"$R3D_PYTHON" "$M/tools/doc_tables.py" "$W" "$TABLES"
tables_seconds=$(($(date +%s) - tables_start))
echo "CPU table measurements added: $tables_seconds seconds"
echo "$tables_seconds" > "$W/tables-seconds.txt"

"$R3D_PYTHON" "$M/tools/doc_import_examples.py" "$W" "$RENDER" > "$W/import-examples.log" 2>&1
