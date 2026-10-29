#!/bin/sh
#
# The demo scene's images for docs/images/, run by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images_demo.sh <out-tree> <work-dir> SCENE.scene.toml OBJECT [--meshlets-only|--small-fill-only|--present-only]
#
# OBJECT is the full renderer; the scene must also place OBJECT_lite,
# OBJECT_flat, OBJECT_fitted, OBJECT_fitted_full and OBJECT_flat_fitted.
# The scene's first renderer is its showcase: the images that show the
# scene rather than compare its variants draw that one.
#
# <out-tree> holds images in render/ and measured blocks in tables/.
# --meshlets-only makes only the meshlet sheet; --present-only makes only the
# three camera present GIFs (nearest, scanline, double) of the showcase.
#
# Run from the repository root with $PYTHON set to a Python that has Pillow and numpy
# (the fidelity sheet also needs launcher/tools/r3d/requirements.txt, found by find_r3d_python);
# ffmpeg must be on PATH. Renderer output goes to logs under <work-dir>.

set -eu

. scripts/lib/run.sh
. launcher/tools/render/gif_from_avi.sh

[ "$#" = 4 ] || { [ "$#" = 5 ] && { [ "$5" = --meshlets-only ] || [ "$5" = --small-fill-only ] || [ "$5" = --present-only ]; }; } || {
    echo "$0: needs OUT WORK SCENE OBJECT [--meshlets-only|--small-fill-only|--present-only]" >&2
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
SHOWCASE=$(run "$PYTHON" launcher/tools/render/showcase.py "$SCENE")

small_fill_images() {
    cat > "$W/small-fill-looks.tsv" <<EOF
smooth|SMOOTH (4/0 FLAT)|${SHOWCASE}|--small-fill 4,0,0|${ID}-small-fill-smooth|
fast|FAST (12/16 FLAT)|${SHOWCASE}|--small-fill 12,16,0|${ID}-small-fill-fast|${ID}-small-fill-fast-diff
grain|12/16 GRAIN|${SHOWCASE}|--small-fill 12,16,1|${ID}-small-fill-grain|${ID}-small-fill-grain-diff
dither|12/16 DITHER|${SHOWCASE}|--small-fill 12,16,2|${ID}-small-fill-dither|${ID}-small-fill-dither-diff
EOF
    small_fill_first=1
    small_fill_end=56
    small_fill_frames=60
    small_fill_dt=100
    small_fill_fps=6
    small_fill_window="trim=start_frame=$small_fill_first:end_frame=$small_fill_end,setpts=PTS-STARTPTS,fps=$small_fill_fps"
    small_fill_palette="max_colors=256:reserve_transparent=0:stats_mode=full"
    # The table comes in on fd 3: the renderer and ffmpeg inherit stdin and may read it.
    while IFS='|' read -r look label object settings gif diff <&3; do
        # Settings are whitespace-separated command arguments from the table above.
        set -- $settings
        run "$HOST" --scene "$ID" --quarter 1 --frames "$small_fill_frames" --dt "$small_fill_dt" --object "$object" "$@" \
            -o "$W/small-fill-$look.bmp" --video "$W/small-fill-$look.avi" 2> "$W/small-fill-$look.log"
        gif_from_avi "$W/small-fill-$look.avi" "$RENDER/$gif.gif" \
            "$small_fill_window" "$small_fill_palette"
    done 3< "$W/small-fill-looks.tsv"
    if cmp -s "$W/small-fill-smooth.bmp" "$W/small-fill-fast.bmp"; then
        echo "small-fill: --small-fill did not change the render" >&2
        exit 1
    fi
    while IFS='|' read -r look label object settings gif diff <&3; do
        run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/small-fill-$look.mp4" \
            --video "$W/small-fill-smooth.avi" "$W/small-fill-$look.avi" \
            --label-a "SMOOTH" --label-b "$label" --csv "$W/small-fill-$look.frames.csv" \
            > "$W/small-fill-$look-compare.log"
        run "$PYTHON" launcher/tools/render/render_temporal.py \
            --video "$W/small-fill-smooth.avi" "$W/small-fill-$look.avi" \
            --csv "$W/small-fill-$look.frames.csv" >> "$W/small-fill-$look-compare.log"
    done 3< "$W/small-fill-looks.tsv"
    run "$PYTHON" - "$W" <<'PY'
import pathlib
import subprocess
import sys
from PIL import Image
sys.path.insert(0, "launcher/tools/render")
from render_compare import GAIN, heatmap, read_video
work = pathlib.Path(sys.argv[1])
fps, baseline = read_video(work / "small-fill-smooth.avi")
baseline = list(baseline)
for row in (work / "small-fill-looks.tsv").read_text().splitlines():
    look, label, _, _, _, _ = row.split("|")
    if look == "smooth":
        continue
    _, variant = read_video(work / f"small-fill-{look}.avi")
    frames = [heatmap(Image.fromarray(a), Image.fromarray(b), None, GAIN)
              for a, b in zip(baseline, variant)]
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
                    "-pix_fmt", "rgb24", "-s", "%dx%d" % frames[0].size, "-r", str(fps), "-i", "-",
                    "-c:v", "rawvideo", "-pix_fmt", "bgr24", str(work / f"small-fill-{look}-diff.avi")],
                   input=b"".join(frame.tobytes() for frame in frames), check=True)
PY
    while IFS='|' read -r look label object settings gif diff <&3; do
        [ "$look" != smooth ] || continue
        gif_from_avi "$W/small-fill-$look-diff.avi" "$RENDER/$diff.gif" \
            "$small_fill_window" "$small_fill_palette"
    done 3< "$W/small-fill-looks.tsv"
    run "$PYTHON" launcher/tools/render/doc_tables.py "$W" "$TABLES" "$ID" "$FULL" \
        --small-fill-data docs/render/data/small-fill
}

if [ "${5:-}" = --small-fill-only ]; then
    small_fill_images
    exit 0
fi

if [ "${5:-}" != --present-only ]; then
for object in "$FULL" "${FULL}_lite" "${FULL}_fitted"; do
    run "$HOST" --quarter 1 --frames 30 --dt 100 -o "$W/meshlets-$object.bmp" \
        --scene "$ID" --object "$object" --view meshlets 2> "$W/meshlets-$object.log"
done
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/${ID}-meshlets.png" \
    --still-column full "$W/meshlets-$FULL.bmp" \
    --still-column lite "$W/meshlets-${FULL}_lite.bmp" \
    --still-column fitted "$W/meshlets-${FULL}_fitted.bmp"
[ "${5:-}" != --meshlets-only ] || exit 0
small_fill_images
fi

# One looping flythrough per demo target and view, the same three seconds of
# it so the GIFs compare. Each keeps every rendered frame at the
# render size, on 128 colours so smooth shading does not band.
DEMO_GIF_FRAMES=30
DEMO_GIF_DT=100
demo_gif() {
    name=$1
    shift
    run "$HOST" --quarter 1 --frames "$DEMO_GIF_FRAMES" --dt "$DEMO_GIF_DT" \
        -o "$W/$name.bmp" --video "$W/$name.avi" "$@" 2> "$W/$name.log"
    gif_from_avi "$W/$name.avi" "$RENDER/$name.gif" "fps=$((1000 / DEMO_GIF_DT))" \
        "max_colors=128:stats_mode=diff"
}
demo_gif ${ID}-present-nearest --scene "$ID" --object "$SHOWCASE" --present nearest
demo_gif ${ID}-present-scanline --scene "$ID" --object "$SHOWCASE" --present scanline
demo_gif ${ID}-present-double --scene "$ID" --object "$SHOWCASE" --present double
[ "${5:-}" != --present-only ] || exit 0

demo_gif ${ID}-full --scene "$ID" --object "$FULL"
demo_gif ${ID}-lite --scene "$ID" --object "${FULL}_lite"
demo_gif ${ID}-flat --scene "$ID" --object "${FULL}_flat"
demo_gif ${ID}-fitted --scene "$ID" --object "${FULL}_fitted"
demo_gif ${ID}-fitted-full --scene "$ID" --object "${FULL}_fitted_full"
demo_gif ${ID}-flat-fitted --scene "$ID" --object "${FULL}_flat_fitted"
# The scrub window over the same seconds, fitted against its source, halfway through.
run sh launcher/tools/render/scene_viewer.sh "$SCENE" --live --object "${FULL}_fitted" --b-args "--object $FULL" \
    --label-a fitted --label-b source --frames "$DEMO_GIF_FRAMES" --dt "$DEMO_GIF_DT" \
    --shot $((DEMO_GIF_FRAMES / 2)) "$RENDER/${ID}-scrub.png" 2> "$W/scrub.log"
demo_gif ${ID}-depth --scene "$ID" --object "$SHOWCASE" --view depth
demo_gif ${ID}-tiles --scene "$ID" --object "$SHOWCASE" --view tiles
demo_gif ${ID}-motion-vectors --scene "$ID" --object "$SHOWCASE" --view motion
demo_gif ${ID}-meshlets --scene "$ID" --object "$SHOWCASE" --view meshlets

# The targets' differences at the pose the GIFs end on: each pair side by side
# with the amplified difference, and the places they differ most, enlarged.
demo_still() {
    run "$HOST" --quarter 1 --frames 30 --dt 100 -o "$W/still-$1.bmp" --scene "$ID" --object "$2" 2> "$W/still-$1.log"
}
# The pairs, and what each variant is, come from doc_tables.py's VARIANT_NOTES:
# compare-full-lite, compare-full-flat, compare-lite-fitted, compare-full-fitted-full
# and compare-flat-flat-fitted for the demo scene.
run "$PYTHON" launcher/tools/render/doc_tables.py --pairs "$SCENE" "$FULL" > "$W/pairs.txt"
while read -r base object base_slug slug <&3; do
    [ -f "$W/still-$base_slug.bmp" ] || demo_still "$base_slug" "$base"
    [ -f "$W/still-$slug.bmp" ] || demo_still "$slug" "$object"
    pair=compare-$base_slug-$slug
    run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/$pair.png" --crops 3 \
        --label-a "$(echo "$base_slug" | tr - ' ')" --label-b "$(echo "$slug" | tr - ' ')" \
        --row "$(echo "$base_slug | $slug" | tr - ' ')" "$W/still-$base_slug.bmp" "$W/still-$slug.bmp" > "$W/$pair.log"
    # render_compare.py writes no crops where the two renders do not differ; fail
    # here rather than leave the pages linking a missing file.
    [ -f "$RENDER/$pair.crops.png" ] || { echo "$0: $pair has no crops, the renders do not differ." >&2; exit 1; }
done 3< "$W/pairs.txt"

. scripts/lib/python.sh
R3D_PYTHON=$(run find_r3d_python "$PWD")
run "$R3D_PYTHON" launcher/tools/r3d/objects/instances_figure.py "$SCENE" \
    --object "$SHOWCASE" --host "$HOST" --out "$RENDER/${ID}-instances.png"
# The fidelity poses: the source model lit per pixel, skipping the initial pose.
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

# The committed smooth bake at the fidelity poses, scored against the committed
# reference, for the fidelity table and the import-light image.
run "$HOST" --quarter 0 --scene "$ID" --object "$FULL" --frames "$FIDELITY_FRAMES" --dt "$FIDELITY_DT" \
    -o "$W/committed-smooth.bmp" --video "$W/committed-smooth.avi" 2> "$W/committed-smooth.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/committed-smooth-unused.png" \
    --reference-video "$W/committed-smooth.avi" "$REFERENCE" --reference-scale 2 > "$W/committed-smooth-compare.log"
# Every variant of OBJECT the scene places, scored the same way: the fidelity
# table, its chart and the flythrough table rank them by these scores.
for object in $(run "$PYTHON" launcher/tools/render/doc_tables.py --variants "$SCENE" "$FULL"); do
    run "$HOST" --quarter 0 --scene "$ID" --object "$object" --frames "$FIDELITY_FRAMES" --dt "$FIDELITY_DT" \
        -o "$W/variant-$object.bmp" --video "$W/variant-$object.avi" 2> "$W/variant-$object.log"
    run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/variant-$object-unused.png" \
        --reference-video "$W/variant-$object.avi" "$REFERENCE" --reference-scale 2 > "$W/variant-$object-compare.log"
done
# The fidelity sheets: two poses of each pair of variants doc_tables.FIDELITY_SHEETS
# names, each beside the reference with its heatmap on the one colour scale.
# --orphans scans this runner: bake-fidelity-sheet.png, bake-fidelity-full-sheet.png
for image in $(run "$PYTHON" launcher/tools/render/doc_tables.py --fidelity-sheets); do
    set --
    while read -r object label; do
        set -- "$@" --bake "$label" "$W/variant-$object.avi"
    done <<EOF
$(run "$PYTHON" launcher/tools/render/doc_tables.py --fidelity-sheet "$SCENE" "$FULL" "$image")
EOF
    run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$image-unused.png" \
        --reference-bakes "$REFERENCE" --reference-scale 2 --sheet-frames 2,4 "$@" \
        --reference-sheet "$RENDER/$image.png" > "$W/$image.log"
done
# bake_and_render DIR MESH OBJECT: bake the scene file in DIR, which keeps its
# import beside a link to the mesh source, pack MESH in place of the committed
# one and render the fidelity poses to DIR.avi.
bake_and_render() {
    dir=$1 mesh=$2 object=$3
    run ln -sfn "$PWD/$DEMO/source" "$dir/source"
    run "$R3D_PYTHON" launcher/tools/r3d/mesh_import.py "$dir/$ID.scene.toml" --mesh "$mesh" --out "$dir" > "$dir/bake.log" 2>&1
    run "$PYTHON" launcher/tools/r3d/build_pack.py -o "$dir/assets" "$SCENE" --replace "$mesh=$dir/$mesh.mesh"
    AUTANA_ASSET_DIR="$dir/assets" run "$HOST" --quarter 0 --scene "$ID" --object "$object" --frames "$FIDELITY_FRAMES" --dt "$FIDELITY_DT" \
        -o "$dir/frame.bmp" --video "$dir.avi" 2> "$dir/render.log"
}
# The indirect-light studies start from the physical look: the committed scene
# without its occlusion and its [indirect] table (physical_scene.py). Their
# sheets on the showcase are fitted, so the GPU stage makes them; the scores of
# the full, lite and flat bakes here fill the indirect tables.
# variant_bake NAME LOOK [MESH OBJECT]: LOOK names physical_scene.py's LOOKS.
variant_bake() {
    run mkdir -p "$W/indirect-$1"
    run cp "$DEMO/$ID.import.toml" "$W/indirect-$1/"
    run "$R3D_PYTHON" launcher/tools/render/physical_scene.py "$DEMO/$ID.scene.toml" "$W/indirect-$1/$ID.scene.toml" --look "$2"
    bake_and_render "$W/indirect-$1" "${3:-$MESH}" "${4:-$FULL}"
}
variant_bake smooth smooth
variant_bake direct direct
variant_bake intensity-2 intensity-2
variant_bake intensity-3 intensity-3
variant_bake boost-2 boost-2
# The physical reference: the source lit per pixel by the scene without its look.
PHYSICAL_REFERENCE=$(run sh launcher/tools/render/render_compare.sh --reference-frames \
    --reference "$W/indirect-smooth/$ID.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/indirect-smooth/reference.log")

# Each bake scored against the physical reference, and each look against its
# own reference too: the scene's [indirect] table reaches the reference, so the
# error against it is the bake's alone.
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/indirect-compare.png" \
    --reference-bakes "$PHYSICAL_REFERENCE" --reference-scale 2 --sheet-frames 2,4 \
    --bake "direct light only" "$W/indirect-direct.avi" --bake "two bounces" "$W/indirect-smooth.avi" > "$W/indirect-compare.log"
look_reference() {
    run sh launcher/tools/render/render_compare.sh --reference-frames \
        --reference "$W/indirect-$1/$ID.scene.toml" --poses "$W/fidelity-poses.txt" 2> "$W/indirect-$1/reference.log"
}
INTENSITY_2_REFERENCE=$(look_reference intensity-2)
INTENSITY_3_REFERENCE=$(look_reference intensity-3)
BOOST_2_REFERENCE=$(look_reference boost-2)
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/indirect-look.png" \
    --reference-bakes "$PHYSICAL_REFERENCE" --reference-scale 2 --sheet-frames 4 \
    --bake "intensity 1" "$W/indirect-smooth.avi" --bake "intensity 2" "$W/indirect-intensity-2.avi" \
    --bake "intensity 3" "$W/indirect-intensity-3.avi" --bake "albedo boost 2" "$W/indirect-boost-2.avi" \
    --bake-reference "intensity 1" "$PHYSICAL_REFERENCE" --bake-reference "intensity 2" "$INTENSITY_2_REFERENCE" \
    --bake-reference "intensity 3" "$INTENSITY_3_REFERENCE" --bake-reference "albedo boost 2" "$BOOST_2_REFERENCE" \
    > "$W/indirect-look.log"

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
variant_bake direct-lite direct "$ID.${FULL}_lite" "${FULL}_lite"
variant_bake direct-flat direct "$ID.${FULL}_flat" "${FULL}_flat"
variant_bake smooth-lite smooth "$ID.${FULL}_lite" "${FULL}_lite"
variant_bake smooth-flat smooth "$ID.${FULL}_flat" "${FULL}_flat"
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
"$R3D_PYTHON" "launcher/tools/render/doc_tables.py" "$W" "$TABLES" "$ID" "$FULL" --scene "$SCENE"
# The same numbers as charts: ${ID}-fidelity.png, ${ID}-indirect.png, ${ID}-flat-sampling.png, ${ID}-indirect-look.png.
"$R3D_PYTHON" launcher/tools/render/bake_charts.py "$TABLES" "$RENDER" "$ID"
tables_seconds=$(($(date +%s) - tables_start))
echo "CPU table measurements added: $tables_seconds seconds"
echo "$tables_seconds" > "$W/tables-seconds.txt"

"$R3D_PYTHON" "launcher/tools/render/doc_import_examples.py" "$SCENE" "$FULL" "$HOST" "$W" "$RENDER" "$FIDELITY_FRAMES" "$FIDELITY_DT" > "$W/import-examples.log" 2>&1
