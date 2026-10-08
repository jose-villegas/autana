#!/bin/sh
#
# The demo scene's images for docs/images/, run by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images_demo.sh <out-tree> <work-dir> SCENE.scene.toml OBJECT [--meshlets-only]
#
# OBJECT is the full renderer; the scene must also place OBJECT_lite,
# OBJECT_flat, OBJECT_fitted, OBJECT_fitted_full and OBJECT_flat_fitted.
#
# <out-tree> holds images in render/ and measured blocks in tables/.
#
# Run from the repository root with $PYTHON set to a Python that has Pillow and numpy
# (the fidelity sheet also needs launcher/tools/r3d/requirements.txt, found by find_r3d_python);
# ffmpeg must be on PATH. Renderer output goes to logs under <work-dir>.

set -eu

. scripts/lib/run.sh

[ "$#" = 4 ] || { [ "$#" = 5 ] && [ "$5" = --meshlets-only ]; } || {
    echo "$0: needs OUT WORK SCENE OBJECT [--meshlets-only]" >&2
    exit 2
}
SCENE=$3
DEMO=$(dirname "$SCENE")
ID=$(basename "$SCENE" .scene.toml)
FULL=$4
RENDER=$1/render
TABLES=$1/tables
W=$2
run mkdir -p "$W" "$RENDER"
build=$(run sh launcher/tools/render/scene_viewer.sh "$SCENE" --build-only -o "$W")
HOST=${build#built }

run "$HOST" --quarter 1 --frames 30 --dt 100 -o "$W/meshlets.bmp" \
    --scene "$ID" --object "$FULL" --view meshlets 2> "$W/meshlets.log"
run "$PYTHON" -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' \
    "$W/meshlets.bmp" "$RENDER/${ID}-meshlets.png"
[ "${5:-}" != --meshlets-only ] || exit 0

# One looping flythrough per demo target and view, the same three seconds of
# it so the GIFs compare, small enough to sit side by side in a table.
demo_gif() {
    name=$1
    shift
    run "$HOST" --quarter 1 --frames 30 --dt 100 \
        -o "$W/$name.bmp" --video "$W/$name.avi" "$@" 2> "$W/$name.log"
    run ffmpeg -hide_banner -loglevel error -y -i "$W/$name.avi" \
        -vf "fps=8,scale=240:-1:flags=lanczos,palettegen=max_colors=64:stats_mode=diff" "$W/$name-palette.png"
    run ffmpeg -hide_banner -loglevel error -y -i "$W/$name.avi" -i "$W/$name-palette.png" \
        -filter_complex "[0:v]fps=8,scale=240:-1:flags=lanczos[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle" \
        -loop 0 "$RENDER/$name.gif"
}
demo_gif ${ID}-full --scene "$ID" --object "$FULL"
demo_gif ${ID}-lite --scene "$ID" --object "${FULL}_lite"
demo_gif ${ID}-flat --scene "$ID" --object "${FULL}_flat"
demo_gif ${ID}-fitted --scene "$ID" --object "${FULL}_fitted"
demo_gif ${ID}-fitted-full --scene "$ID" --object "${FULL}_fitted_full"
demo_gif ${ID}-flat-fitted --scene "$ID" --object "${FULL}_flat_fitted"
demo_gif ${ID}-depth --scene "$ID" --object "$FULL" --view depth
demo_gif ${ID}-tiles --scene "$ID" --object "$FULL" --view tiles
demo_gif ${ID}-motion-vectors --scene "$ID" --object "$FULL" --view motion

# The targets' differences at the pose the GIFs end on: each pair side by side
# with the amplified difference, and the places they differ most, enlarged.
demo_still() {
    run "$HOST" --quarter 1 --frames 30 --dt 100 -o "$W/still-$1.bmp" --scene "$ID" --object "$2" 2> "$W/still-$1.log"
}
demo_still full "$FULL"
demo_still lite "${FULL}_lite"
demo_still flat "${FULL}_flat"
demo_still fitted "${FULL}_fitted"
demo_still fitted-full "${FULL}_fitted_full"
demo_still flat-fitted "${FULL}_flat_fitted"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-lite.png" --crops 3 \
    --label-a full --label-b lite --row "full | lite" "$W/still-full.bmp" "$W/still-lite.bmp" > "$W/compare-full-lite.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-flat.png" --crops 3 \
    --label-a smooth --label-b flat --row "smooth | flat" "$W/still-full.bmp" "$W/still-flat.bmp" > "$W/compare-full-flat.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-lite-fitted.png" --crops 3 \
    --label-a lite --label-b fitted --row "lite | fitted" "$W/still-lite.bmp" "$W/still-fitted.bmp" > "$W/compare-lite-fitted.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-full-fitted-full.png" --crops 3 \
    --label-a full --label-b "fitted full" --row "full | fitted full" "$W/still-full.bmp" "$W/still-fitted-full.bmp" > "$W/compare-full-fitted-full.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/compare-flat-flat-fitted.png" --crops 3 \
    --label-a flat --label-b "flat fitted" --row "flat | flat fitted" "$W/still-flat.bmp" "$W/still-flat-fitted.bmp" > "$W/compare-flat-flat-fitted.log"
# render_compare.py writes no crops where the two renders do not differ; fail
# here rather than leave the pages linking a missing file.
for crops in compare-full-lite compare-full-flat compare-lite-fitted compare-full-fitted-full compare-flat-flat-fitted; do
    [ -f "$RENDER/$crops.crops.png" ] || { echo "$0: $crops has no crops, the renders do not differ." >&2; exit 1; }
done

. scripts/lib/python.sh
R3D_PYTHON=$(run find_r3d_python "$PWD")
# The fidelity sheet: two poses of the committed flat bake against the source
# model lit per pixel. The reference skips the initial pose.
# The source model lives in the demo folder and uses Git LFS.
FIDELITY_FRAMES=5
FIDELITY_DT=5000
run "$R3D_PYTHON" -c 'import sys; from pathlib import Path; sys.path.insert(0, "launcher/tools"); from r3d.import_settings import load_scene; from r3d.mesh_import import fidelity_poses; sys.stdout.buffer.write(fidelity_poses(load_scene(Path(sys.argv[1])), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])).encode())' \
    "$SCENE" "$FULL" "$FIDELITY_FRAMES" "$FIDELITY_DT" > "$W/fidelity-poses.txt"
MESH=$(run "$PYTHON" -c 'import sys; from pathlib import Path; sys.path.insert(0, "launcher/tools"); from r3d.import_settings import load_scene; print(next(job.asset_name for job in load_scene(Path(sys.argv[1])).renderers if job.object.name == sys.argv[2]))' "$SCENE" "$FULL")
# render_compare.sh keeps the traced frames in r3d/.cache/reference by a hash of
# their inputs, so only a change to the scene, tracer or poses traces again.
REFERENCE=$(run sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "$DEMO/$ID.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/fidelity-reference.log")
run "$HOST" --quarter 0 --scene "$ID" --object "${FULL}_flat" --frames "$FIDELITY_FRAMES" --dt "$FIDELITY_DT" \
    -o "$W/fidelity-flat.bmp" --video "$W/fidelity-flat.avi" 2> "$W/fidelity-flat.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/fidelity-unused.png" \
    --reference-video "$W/fidelity-flat.avi" "$REFERENCE" --reference-scale 2 \
    --reference-sheet "$RENDER/bake-fidelity-sheet.png" --sheet-frames 2,4 --label-a "flat bake" > "$W/fidelity-compare.log"

# The committed smooth bake at the fidelity poses, scored against the committed
# reference, for the fidelity table and the import-light image.
run "$HOST" --quarter 0 --scene "$ID" --object "$FULL" --frames "$FIDELITY_FRAMES" --dt "$FIDELITY_DT" \
    -o "$W/committed-smooth.bmp" --video "$W/committed-smooth.avi" 2> "$W/committed-smooth.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/committed-smooth-unused.png" \
    --reference-video "$W/committed-smooth.avi" "$REFERENCE" --reference-scale 2 > "$W/committed-smooth-compare.log"
# bake_and_render DIR MESH OBJECT: bake the scene file in DIR, which keeps its
# import beside a link to the mesh source, pack MESH in place of the committed
# one and render the fidelity poses to DIR.avi.
bake_and_render() {
    dir=$1 mesh=$2 object=$3
    run ln -sfn "$PWD/$DEMO/source" "$dir/source"
    run "$R3D_PYTHON" launcher/tools/r3d/mesh_import.py "$dir/$ID.scene.toml" --mesh "$mesh" > "$dir/bake.log" 2>&1
    run "$PYTHON" launcher/tools/r3d/build_pack.py -o "$dir/assets" "$SCENE" --replace "$mesh=$dir/$mesh.mesh"
    AUTANA_ASSET_DIR="$dir/assets" run "$HOST" --quarter 0 --scene "$ID" --object "$object" --frames "$FIDELITY_FRAMES" --dt "$FIDELITY_DT" \
        -o "$dir/frame.bmp" --video "$dir.avi" 2> "$dir/render.log"
}
# The indirect-light and occlusion studies start from the physical look: the
# committed scene without its occlusion and its [indirect] table, which the
# scene sets for its own renders. Everything else here uses the committed look.
run "$R3D_PYTHON" "launcher/tools/render/physical_scene.py" "$DEMO/$ID.scene.toml" "$W/physical.scene.toml"
# variant_bake NAME BOUNCES SCENE-TABLE [MESH OBJECT]: bounces is `keep`, or `none` to take
# the scene's `[bake].indirect` out; the table goes before the first object.
variant_bake() {
    run mkdir -p "$W/indirect-$1"
    run cp "$DEMO/$ID.import.toml" "$W/indirect-$1/"
    run awk -v table="$3" -v direct="$2" '/^\[\[objects\]\]/ && !done { if (table != "") print table "\n"; done = 1 }
        direct == "none" && /^indirect = \{/ { next } { print }' \
        "$W/physical.scene.toml" > "$W/indirect-$1/$ID.scene.toml"
    bake_and_render "$W/indirect-$1" "${4:-$MESH}" "${5:-$FULL}"
}
variant_bake smooth keep ''
variant_bake direct none ''
variant_bake intensity-2 keep '[indirect]\nintensity = 2.0'
variant_bake intensity-3 keep '[indirect]\nintensity = 3.0'
variant_bake boost-2 keep '[indirect]\nalbedo_boost = 2.0'
# The physical reference: the source lit per pixel by the scene without its look.
PHYSICAL_REFERENCE=$(run sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "$W/indirect-smooth/$ID.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/indirect-smooth/reference.log")

# Reference, direct-only bake and two-bounce bake at two poses, with each
# bake's dE heatmap against the reference, then the places the two bakes differ
# most with the reference above them.
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/indirect-compare.png" --crops 4 \
    --reference-bakes "$PHYSICAL_REFERENCE" --reference-scale 2 --sheet-frames 2,4 \
    --bake "direct light only" "$W/indirect-direct.avi" --bake "two bounces" "$W/indirect-smooth.avi" > "$W/indirect-compare.log"
[ -f "$W/indirect-compare.crops.png" ] || { echo "$0: indirect light has no crops, the bakes do not differ." >&2; exit 1; }
run cp "$W/indirect-compare.png" "$RENDER/bake-indirect-compare.png"
run cp "$W/indirect-compare.crops.png" "$RENDER/bake-indirect-crops.png"

# The look controls: the physical bake, indirect intensity 2 and 3, and an
# albedo boost of 2, last pose, beside the physical reference and, under it,
# each look's own reference: the scene's [indirect] table reaches the reference
# too, so the error against it is the bake's alone.
look_reference() {
    run sh launcher/tools/render/render_compare.sh --reference-frames \
        --reference "$W/indirect-$1/$ID.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/indirect-$1/reference.log"
}
INTENSITY_2_REFERENCE=$(look_reference intensity-2)
INTENSITY_3_REFERENCE=$(look_reference intensity-3)
BOOST_2_REFERENCE=$(look_reference boost-2)
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/bake-indirect-look.png" \
    --reference-bakes "$PHYSICAL_REFERENCE" --reference-scale 2 --sheet-frames 4 \
    --bake "intensity 1" "$W/indirect-smooth.avi" --bake "intensity 2" "$W/indirect-intensity-2.avi" \
    --bake "intensity 3" "$W/indirect-intensity-3.avi" --bake "albedo boost 2" "$W/indirect-boost-2.avi" \
    --bake-reference "intensity 1" "$PHYSICAL_REFERENCE" --bake-reference "intensity 2" "$INTENSITY_2_REFERENCE" \
    --bake-reference "intensity 3" "$INTENSITY_3_REFERENCE" --bake-reference "albedo boost 2" "$BOOST_2_REFERENCE" \
    > "$W/indirect-look.log"

# The scene's ambient light is faint, so the occlusion has little to scale; both
# bakes raise it. ao_bake NAME AO-LINE: the line goes after `[bake].indirect`.
ao_bake() {
    run mkdir -p "$W/ao-$1"
    run cp "$DEMO/$ID.import.toml" "$W/ao-$1/"
    run awk -v ao="$2" '/^\[/ { ambient = ($0 == "[ambient]") } ambient && /^intensity = / { print "intensity = 0.25"; next }
        { print } /^indirect = \{/ && ao != "" { print ao }' \
        "$W/physical.scene.toml" > "$W/ao-$1/$ID.scene.toml"
    bake_and_render "$W/ao-$1" "$MESH" "$FULL"
}
ao_bake flat ''
ao_bake occluded 'ao = { distance = 80.0, rays = 32 }'
AO_REFERENCE=$(run sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "$W/ao-occluded/$ID.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/ao-occluded/reference.log")
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/ao-compare.png" --crops 4 \
    --reference-bakes "$AO_REFERENCE" --reference-scale 2 --sheet-frames 2,4 \
    --bake "no occlusion" "$W/ao-flat.avi" --bake "occlusion" "$W/ao-occluded.avi" > "$W/ao-compare.log"
[ -f "$W/ao-compare.crops.png" ] || { echo "$0: occlusion has no crops, the bakes do not differ." >&2; exit 1; }
run cp "$W/ao-compare.png" "$RENDER/bake-ao-compare.png"
run cp "$W/ao-compare.crops.png" "$RENDER/bake-ao-crops.png"

# The occlusion factor beside the reference frame at the sheet's two poses.
run "$R3D_PYTHON" launcher/tools/r3d/reference_render.py "$W/ao-occluded/$ID.scene.toml" --poses "$W/fidelity-poses.txt" \
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
#   against_reference <object> <heatmap image> <reference image> <label> [--sheet] [--crops]
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
            *) echo "$0: against_reference unknown option $1" >&2; exit 2 ;;
        esac
        shift
    done
    run "$HOST" --quarter 0 --scene "$ID" --object "$scene" --frames "$FIDELITY_FRAMES" --dt "$FIDELITY_DT" \
        -o "$W/fidelity-$scene.bmp" --video "$W/fidelity-$scene.avi" 2> "$W/fidelity-$scene.log"
    run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$scene-unused.png" \
        --reference-video "$W/fidelity-$scene.avi" "$REFERENCE" --reference-scale 2 \
        --reference-sheet "$RENDER/$heat.png" --sheet-frames 2,4 --label-a "$label" > "$W/$scene-compare.log"
    if [ "$crops" = yes ]; then
        run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$reference.png" --crops 3 \
            --label-a "$label" --label-b reference --row "$label | reference" "$W/fidelity-$scene.bmp" "$W/fidelity-reference-4.png" > "$W/$scene-reference.log"
        cp "$W/$reference.crops.png" "$RENDER/" || {
            echo "$0: $scene matches the reference, no crops." >&2
            exit 1
        }
    fi
    [ "$sheet" = no ] || cp "$W/$reference.png" "$RENDER/"
}
against_reference "${FULL}_fitted" appearance-chosen-heat appearance-chosen-reference fitted --crops
against_reference "${FULL}_fitted_full" appearance-fitted-full-heat appearance-fitted-full-reference "fitted full" --sheet --crops
against_reference "${FULL}_lite" appearance-lite-reference appearance-lite-reference-row simplifier
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/appearance-lite-fitted-reference.crops.png" --crops 3 \
    --label-a simplifier --label-b fitted --reference-crops "$W/fidelity-reference-4.png" \
    "$W/fidelity-${FULL}_lite.bmp" "$W/fidelity-${FULL}_fitted.bmp" > "$W/appearance-lite-fitted-reference.log"

# The lite and flat meshes with and without bounced light, at the same poses and
# against the same physical reference as the full mesh above, so one table holds
# one look.
tables_start=$(date +%s)
variant_bake direct-lite none '' "$ID.${FULL}_lite" "${FULL}_lite"
variant_bake direct-flat none '' "$ID.${FULL}_flat" "${FULL}_flat"
variant_bake smooth-lite keep '' "$ID.${FULL}_lite" "${FULL}_lite"
variant_bake smooth-flat keep '' "$ID.${FULL}_flat" "${FULL}_flat"
for kind in lite flat; do
    for look in direct smooth; do
        "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$look-$kind-unused.png" \
            --reference-video "$W/indirect-$look-$kind.avi" "$PHYSICAL_REFERENCE" --reference-scale 2 > "$W/$look-$kind-compare.log"
    done
done

sweep_start=$(date +%s)
"$R3D_PYTHON" launcher/tools/r3d/bake_fidelity.py "$DEMO/$ID.scene.toml" --mesh "${FULL}_flat" \
    --host "$HOST" \
    --render-args "--quarter 0 --scene $ID --object ${FULL}_flat --frames $FIDELITY_FRAMES --dt $FIDELITY_DT" \
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
    > "$W/sampling.log" 2>&1
sweep_seconds=$(($(date +%s) - sweep_start))
echo "CPU flat sampling sweep: $sweep_seconds seconds"
echo "$sweep_seconds" > "$W/sweep-seconds.txt"
"$R3D_PYTHON" "launcher/tools/render/doc_tables.py" "$W" "$TABLES" "$ID" "$FULL"
tables_seconds=$(($(date +%s) - tables_start))
echo "CPU table measurements added: $tables_seconds seconds"
echo "$tables_seconds" > "$W/tables-seconds.txt"

"$R3D_PYTHON" "launcher/tools/render/doc_import_examples.py" "$SCENE" "$FULL" "$HOST" "$W" "$RENDER" "$FIDELITY_FRAMES" "$FIDELITY_DT" > "$W/import-examples.log" 2>&1
