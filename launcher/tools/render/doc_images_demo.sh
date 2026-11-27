#!/bin/sh
#
# The demo scene's images for docs/images/, run by
# launcher/tools/render/render_doc_images.sh.
#
#   doc_images_demo.sh <out-tree> <work-dir> SCENE.scene.toml OBJECT [--small-fill-only|--present-only]
#
# OBJECT is the scene's fitted renderer, the one it ships. The scene's first
# renderer is its showcase: the images that show the scene draw that one.
#
# <out-tree> holds images in render/ and measured blocks in tables/.
# --small-fill-only makes only the small-fill looks; --present-only makes only
# the three camera present GIFs (nearest, scanline, double) of the showcase.
#
# Run from the repository root with $PYTHON set to a Python that has Pillow and numpy
# (the fidelity sheet also needs launcher/tools/r3d/requirements.txt, found by find_r3d_python);
# The comparison videos use the pinned ffmpeg. Renderer output goes to logs under <work-dir>.

set -eu

. scripts/lib/run.sh

[ "$#" = 4 ] || { [ "$#" = 5 ] && { [ "$5" = --small-fill-only ] || [ "$5" = --present-only ]; }; } || {
    echo "$0: needs OUT WORK SCENE OBJECT [--small-fill-only|--present-only]" >&2
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
smooth|SMOOTH (4/0 FLAT)|${SHOWCASE}|--set render.small_fill.ceiling=4 --set render.small_fill.gate=0 --set render.small_fill.style=0|${ID}-small-fill-smooth|
fast|FAST (12/16 FLAT)|${SHOWCASE}|--set render.small_fill.ceiling=12 --set render.small_fill.gate=16 --set render.small_fill.style=0|${ID}-small-fill-fast|${ID}-small-fill-fast-diff
grain|12/16 GRAIN|${SHOWCASE}|--set render.small_fill.ceiling=12 --set render.small_fill.gate=16 --set render.small_fill.style=1|${ID}-small-fill-grain|${ID}-small-fill-grain-diff
dither|12/16 DITHER|${SHOWCASE}|--set render.small_fill.ceiling=12 --set render.small_fill.gate=16 --set render.small_fill.style=2|${ID}-small-fill-dither|${ID}-small-fill-dither-diff
EOF
    small_fill_first=1
    small_fill_end=56
    small_fill_frames=60
    small_fill_dt=100
    small_fill_fps=6
    # The table comes in on fd 3: the renderer and Python inherit stdin and may read it.
    while IFS='|' read -r look label object settings gif diff <&3; do
        # Settings are whitespace-separated command arguments from the table above.
        set -- $settings
        run "$HOST" --scene "$ID" --quarter 1 --frames "$small_fill_frames" --dt "$small_fill_dt" --object "$object" "$@" \
            -o "$W/small-fill-$look.bmp" --video "$W/small-fill-$look.avi" 2> "$W/small-fill-$look.log"
        run "$PYTHON" launcher/tools/docs/figures.py gif "$W/small-fill-$look.avi" "$RENDER/$gif.gif" \
            --first "$small_fill_first" --end "$small_fill_end" --fps "$small_fill_fps"
    done 3< "$W/small-fill-looks.tsv"
    if cmp -s "$W/small-fill-smooth.bmp" "$W/small-fill-fast.bmp"; then
        echo "small-fill: render.small_fill did not change the render" >&2
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
    run "$PYTHON" - "$W" "$RENDER" "$small_fill_first" "$small_fill_end" "$small_fill_fps" <<'PY'
import pathlib
import sys
from PIL import Image
sys.path[:0] = ["launcher/tools", "launcher/tools/render"]
from docs.figures import clip, save_gif
from check_avi import read_video
from render_compare import GAIN, heatmap
work, render = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
first, end, fps = map(int, sys.argv[3:6])
rate, baseline = read_video(work / "small-fill-smooth.avi")
baseline = list(baseline)
for row in (work / "small-fill-looks.tsv").read_text().splitlines():
    look, _, _, _, _, diff = row.split("|")
    if look == "smooth":
        continue
    _, variant = read_video(work / f"small-fill-{look}.avi")
    frames = [heatmap(Image.fromarray(a), Image.fromarray(b), None, GAIN) for a, b in zip(baseline, variant)]
    save_gif(clip(frames, rate, fps, first=first, end=end), render / f"{diff}.gif", 1000 / fps)
PY
    run "$PYTHON" launcher/tools/render/doc_tables.py "$W" "$TABLES" "$ID" "$FULL" \
        --small-fill-data docs/render/data/small-fill
}

if [ "${5:-}" = --small-fill-only ]; then
    small_fill_images
    exit 0
fi

[ "${5:-}" = --present-only ] || small_fill_images

# One looping flythrough per present mode and view, the same three seconds of
# it so the GIFs compare. Each keeps every rendered frame at the
# render size.
DEMO_GIF_FRAMES=30
DEMO_GIF_DT=100
demo_gif() {
    name=$1
    shift
    run "$HOST" --quarter 1 --frames "$DEMO_GIF_FRAMES" --dt "$DEMO_GIF_DT" \
        -o "$W/$name.bmp" --video "$W/$name.avi" "$@" 2> "$W/$name.log"
    run "$PYTHON" launcher/tools/docs/figures.py gif "$W/$name.avi" "$RENDER/$name.gif" --fps "$((1000 / DEMO_GIF_DT))"
}
demo_gif ${ID}-present-nearest --scene "$ID" --object "$SHOWCASE" --present nearest
demo_gif ${ID}-present-scanline --scene "$ID" --object "$SHOWCASE" --present scanline
demo_gif ${ID}-present-double --scene "$ID" --object "$SHOWCASE" --present double
[ "${5:-}" != --present-only ] || exit 0

demo_gif ${ID}-depth --scene "$ID" --object "$SHOWCASE" --view depth
demo_gif ${ID}-tiles --scene "$ID" --object "$SHOWCASE" --view tiles
demo_gif ${ID}-meshlets --scene "$ID" --object "$SHOWCASE" --view meshlets
# One still of each view the renderer declares, at the pose the GIFs end on:
# View-Modes' gallery. A --view it does not know makes it list them.
views=$("$HOST" --scene "$ID" --view none 2>&1 >/dev/null | sed -n 's/^scene_viewer: --view is \(.*\), not none$/\1/p' | sed 's/, / /g')
[ -n "$views" ] || { echo "$0: the renderer listed no views" >&2; exit 1; }
for view in $views; do
    run "$HOST" --quarter 1 --frames "$DEMO_GIF_FRAMES" --dt "$DEMO_GIF_DT" --scene "$ID" --object "$SHOWCASE" \
        --view "$view" -o "$W/view-$view.bmp" 2> "$W/view-$view.log"
    run "$PYTHON" launcher/tools/docs/figures.py png "$W/view-$view.bmp" "$RENDER/${ID}-view-$view.png"
done

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

# The committed bake at the fidelity poses, scored against the committed
# reference, for the fidelity table, the appearance sheets and the import-light image.
run "$HOST" --quarter 0 --scene "$ID" --object "$FULL" --frames "$FIDELITY_FRAMES" --dt "$FIDELITY_DT" \
    -o "$W/committed-smooth.bmp" --video "$W/committed-smooth.avi" 2> "$W/committed-smooth.log"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/committed-smooth-unused.png" \
    --reference-video "$W/committed-smooth.avi" "$REFERENCE" --reference-scale 2 > "$W/committed-smooth-compare.log"
# bake_and_render SCENE DIR MESH OBJECT: bake SCENE, whose import sits beside it,
# into DIR, pack MESH in place of the committed one and render the fidelity
# poses to DIR.avi.
bake_and_render() {
    scene=$1 dir=$2 mesh=$3 object=$4
    run "$R3D_PYTHON" launcher/tools/r3d/mesh_import.py "$scene" --mesh "$mesh" --out "$dir" > "$dir/bake.log" 2>&1
    run "$PYTHON" launcher/tools/r3d/build_pack.py -o "$dir/assets" "$SCENE" --replace "$mesh=$dir/$mesh.bundle"
    AUTANA_ASSET_DIR="$dir/assets" run "$HOST" --quarter 0 --scene "$ID" --object "$object" --frames "$FIDELITY_FRAMES" --dt "$FIDELITY_DT" \
        -o "$dir/frame.bmp" --video "$dir.avi" 2> "$dir/render.log"
}
# The indirect-light studies start from the physical look: the committed scene
# without its occlusion and its [indirect] table (physical_scene.py). Their
# sheets on the showcase are fitted, so the GPU stage makes them; the scores of
# OBJECT's CPU bakes here fill the indirect tables.
# look_bake NAME LOOK: LOOK names physical_scene.py's LOOKS.
look_bake() {
    run mkdir -p "$W/indirect-$1"
    run cp "$DEMO/$ID.import.toml" "$W/indirect-$1/"
    run ln -sfn "$PWD/$DEMO/source" "$W/indirect-$1/source"
    run "$R3D_PYTHON" launcher/tools/render/physical_scene.py "$DEMO/$ID.scene.toml" "$W/indirect-$1/$ID.scene.toml" --look "$2"
    bake_and_render "$W/indirect-$1/$ID.scene.toml" "$W/indirect-$1" "$MESH" "$FULL"
}
look_bake smooth smooth
look_bake direct direct
look_bake intensity-2 intensity-2
look_bake intensity-3 intensity-3
look_bake boost-2 boost-2
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

# The simplifier against the fit: OBJECT's CPU start, the import simplified to
# the variant's shared shape error, baked from the committed scene in place, so the paths it
# names resolve, beside the committed fit.
run mkdir -p "$W/start"
bake_and_render "$SCENE" "$W/start" "$MESH" "$FULL"
#   against_reference <video> <heatmap image> <label>
against_reference() {
    run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/$2-unused.png" \
        --reference-video "$1" "$REFERENCE" --reference-scale 2 \
        --reference-sheet "$RENDER/$2.png" --sheet-frames 2,4 --label-a "$3" > "$W/$2.log"
}
against_reference "$W/committed-smooth.avi" appearance-chosen-heat fitted
against_reference "$W/start.avi" appearance-lite-reference simplifier
run "$PYTHON" -c 'import pathlib, sys; from PIL import Image
frame = sorted(pathlib.Path(sys.argv[1]).glob("*.png"))[4]
picture = Image.open(frame).convert("RGB")
picture.resize((picture.width * 2, picture.height * 2), Image.Resampling.NEAREST).save(sys.argv[2])' "$REFERENCE" "$W/fidelity-reference-4.png"
run "$PYTHON" launcher/tools/render/render_compare.py --out "$RENDER/appearance-lite-fitted-reference.crops.png" --crops 3 \
    --label-a simplifier --label-b fitted --reference-crops "$W/fidelity-reference-4.png" \
    "$W/start/frame.bmp" "$W/committed-smooth.bmp" > "$W/appearance-lite-fitted-reference.log"
# Two poses of the simplifier and the fit, each beside the reference with its
# heatmap on the one colour scale.
run "$PYTHON" launcher/tools/render/render_compare.py --out "$W/bake-fidelity-full-sheet-unused.png" \
    --reference-bakes "$REFERENCE" --reference-scale 2 --sheet-frames 2,4 \
    --bake simplifier "$W/start.avi" --bake fitted "$W/committed-smooth.avi" \
    --reference-sheet "$RENDER/bake-fidelity-full-sheet.png" > "$W/bake-fidelity-full-sheet.log"

"$R3D_PYTHON" "launcher/tools/render/doc_tables.py" "$W" "$TABLES" "$ID" "$FULL"
# The same numbers as charts: ${ID}-fidelity.png, ${ID}-indirect.png, ${ID}-indirect-look.png.
"$R3D_PYTHON" launcher/tools/render/bake_charts.py "$TABLES" "$RENDER" "$ID"

"$R3D_PYTHON" "launcher/tools/render/doc_import_examples.py" "$SCENE" "$FULL" "$HOST" "$W" "$RENDER" "$FIDELITY_FRAMES" "$FIDELITY_DT" > "$W/import-examples.log" 2>&1
