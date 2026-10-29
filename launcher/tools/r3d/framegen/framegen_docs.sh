#!/bin/sh
#
# Regenerate docs/render/Frame-Generation.md's table, plot, stills and clip
# from the source: builds the host renderer, scores every scene at every
# half-picture size of the dynamic-resolution ladder, and rewrites the
# generated block and the images.
#
#   ./launcher/tools/r3d/framegen/framegen_docs.sh [-j JOBS] [--out IMAGES TABLES]
#
# The board table comes from the framegen_perf suite's capture beside the page,
# docs/render/data/framegen-board.log.
#
# --out writes the images to IMAGES and the tables to TABLES
# instead, leaving the page alone: launcher/tools/render/render_doc_images.sh
# runs it that way.
#
# Run from the repository root with $PYTHON set to a Python with Pillow,
# numpy and matplotlib, and ffmpeg on PATH.

set -eu

JOBS=4
[ "${1:-}" = -j ] && { JOBS=$2; shift 2; }
IMAGES=docs/images/render
TABLES=
[ "${1:-}" = --out ] && { IMAGES=$2; TABLES=$3; shift 3; }

# Each scene key with the scene file and camera whose path it flies.
SCENES="bistro-fitted-full sponza-fitted-full sponza sponza-flat sponza-lite sponza-fitted sponza-flat-fitted"
CAMERA=camera
scene_file() {
    case $1 in
        bistro*) echo launcher/demo/bistro/bistro.scene.toml ;;
        *) echo launcher/demo/sponza/sponza.scene.toml ;;
    esac
}
SIZES="184x224 184x149 184x112"
# The doc's pictures, at the ladder's top half-picture step: for the target
# scene and the reference one, the frame whose warp opens the most holes, and
# a clip around the target scene's.
LEAD=bistro-fitted-full
SECOND=sponza-fitted-full
SHOWN_SIZE=184x224

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
SCORE=launcher/tools/r3d/framegen/framegen_score.py

sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh --build-only -o "$WORK/build" > "$WORK/build.log"
RENDERER="$WORK/build/render_lab_render"
[ -x "$RENDERER" ] || RENDERER="$RENDERER.exe"

mkdir -p "$IMAGES"
for scene in $SCENES; do
    for size in $SIZES; do
        extra=""
        if [ "$scene/$size" = "$LEAD/$SHOWN_SIZE" ]; then
            extra="--still $IMAGES/framegen-bistro-holes.png --clip $IMAGES/framegen-bistro.gif"
        elif [ "$scene/$size" = "$SECOND/$SHOWN_SIZE" ]; then
            extra="--still $IMAGES/framegen-sponza-holes.png"
        fi
        echo "$PYTHON $SCORE --renderer $RENDERER --scene $scene --scene-file $(scene_file "$scene") --camera $CAMERA --size $size --csv $WORK/$scene-$size.csv $extra"
    done
done | xargs -P "$JOBS" -I{} sh -c '{}'

mkdir -p "${TABLES:-$WORK/tables}"
csvs=""
for scene in $SCENES; do
    for size in $SIZES; do
        csvs="$csvs $WORK/$scene-$size.csv"
    done
done
# shellcheck disable=SC2086
"$PYTHON" launcher/tools/r3d/framegen/framegen_report.py --out-table "${TABLES:-$WORK/tables}/framegen-scores.md" \
    --out-plot "$IMAGES/framegen-bistro-path.png" --plot-scene "$LEAD" --plot-size "$SHOWN_SIZE"     --board docs/render/data/framegen-board.log --out-board-table "${TABLES:-$WORK/tables}/framegen-board.md" $csvs
[ -n "$TABLES" ] || "$PYTHON" launcher/tools/render/generated_blocks.py --tables "$WORK/tables"
