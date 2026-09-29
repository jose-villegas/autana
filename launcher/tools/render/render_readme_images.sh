#!/bin/sh
#
# Regenerate every image under docs/images/overview/ from the firmware's own
# host renders, so a doc's pictures are never a hand-refreshed chore.
#
#   ./launcher/tools/render/render_readme_images.sh            # rewrite the images in place
#   ./launcher/tools/render/render_readme_images.sh --check    # only report which changed
#
# Needs a host C compiler, Python with Pillow, and ffmpeg 5 or newer. Runs in
# Git Bash on Windows and on Linux.
#
# --check renders into launcher/tools/results/readme_images/out/ and compares
# each result with the committed image by decoded pixels (compare_images.py),
# never by bytes: two encoder versions write different files for one picture.
# It prints "same <image>" or "changed <image>: <how>" per image and exits 1
# when any changed, 2 when the images could not be made. An image in the
# folder that this script does not make is reported as "orphan" and fails
# the check, so a picture cannot sit outside the refresh.
#
# The scenes themselves are pinned by render_all_scenes.sh; this only turns
# their frames into the files the docs embed.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$TOOLS_DIR/../../.." && pwd)
cd "$ROOT"

IMAGES=docs/images/overview
RESULTS=launcher/tools/results/readme_images
WORK=$RESULTS/work
OUT=$IMAGES

CHECK=0
case "${1:-}" in
    "") ;;
    --check) CHECK=1; OUT=$RESULTS/out ;;
    *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

# Windows has a python3 launcher stub that is not Python; ask for Pillow.
PYTHON=""
for candidate in python3 python; do
    if command -v "$candidate" > /dev/null 2>&1 && "$candidate" -c 'import PIL' > /dev/null 2>&1; then
        PYTHON=$candidate
        break
    fi
done
if [ -z "$PYTHON" ]; then
    echo "No Python with Pillow found (pip install pillow)." >&2
    exit 2
fi
if ! command -v ffmpeg > /dev/null 2>&1; then
    echo "ffmpeg not found." >&2
    exit 2
fi

rm -rf "$RESULTS"
mkdir -p "$WORK" "$OUT"

bmp_to_png() {
    "$PYTHON" -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$1" "$2"
}

# --- launcher: the release build's apps, read from the tree ---------------
L=$WORK/launcher_home
sh launcher/tools/render/scenes/launcher_home_render_host.sh -o "$L" > "$WORK/launcher_home.log"
set --
for dir in launcher/main/apps/*/; do
    [ -f "${dir}development_only.cmake" ] && continue
    for source in "$dir"app_*.c; do
        names=$(sed -n 's/^ *\.name = "\(.*\)",\r\{0,1\}$/\1/p' "$source")
        old_ifs=$IFS
        IFS='
'
        for name in $names; do
            set -- "$@" --row "$name"
        done
        IFS=$old_ifs
    done
done
"$L/launcher_home_render" --quarter 1 "$@" -o "$L/release.bmp" 2> /dev/null
bmp_to_png "$L/release.bmp" "$OUT/launcher-home.png"
# One 4 s rock of the board, 30 degrees either way.
"$L/launcher_home_render" --quarter 1 --tilt-sweep "$@" --frames 250 --dt 16 \
    -o "$L/sweep.bmp" --video "$L/sweep.avi" 2> /dev/null
ffmpeg -hide_banner -loglevel error -y -i "$L/sweep.avi" \
    -vf "fps=15,palettegen=stats_mode=diff" "$L/sweep-palette.png"
ffmpeg -hide_banner -loglevel error -y -i "$L/sweep.avi" -i "$L/sweep-palette.png" \
    -filter_complex "[0:v]fps=15[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle" \
    -loop 0 "$OUT/launcher-home.gif"

# --- render lab: cube, Cornell box, Sponza --------------------------------
R=$WORK/render_lab
sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh -o "$R" > "$WORK/render_lab.log"
bmp_to_png "$R/gouraud-landscape.bmp" "$OUT/render-lab-cube.png"
bmp_to_png "$R/cornell-landscape.bmp" "$OUT/render-lab-cornell.png"
# 100 frames, reversed back onto itself as a loop.
"$R/render_lab_render" --quarter 1 --no-hud --scene gouraud --frames 100 --dt 33 \
    -o "$R/cube-motion.bmp" --video "$R/cube-motion.avi" 2> /dev/null
ffmpeg -hide_banner -loglevel error -y -i "$R/cube-motion.avi" \
    -vf "fps=12,split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1:a=0,palettegen" \
    "$R/cube-palette.png"
ffmpeg -hide_banner -loglevel error -y -i "$R/cube-motion.avi" -i "$R/cube-palette.png" \
    -filter_complex "[0:v]fps=12,split[a][b];[b]reverse[r];[a][r]concat=n=2:v=1:a=0[v];[v][1:v]paletteuse=dither=bayer" \
    -loop 0 "$OUT/render-lab-cube.gif"
# The first 6 s of the flythrough, 8 frames a second.
"$R/render_lab_render" --quarter 1 --no-hud --scene sponza --frames 90 --dt 100 \
    -o "$R/sponza-motion.bmp" --video "$R/sponza-motion.avi" 2> /dev/null
ffmpeg -hide_banner -loglevel error -y -t 6 -i "$R/sponza-motion.avi" \
    -vf "fps=8,palettegen=stats_mode=diff" "$R/sponza-palette.png"
ffmpeg -hide_banner -loglevel error -y -t 6 -i "$R/sponza-motion.avi" -i "$R/sponza-palette.png" \
    -filter_complex "[0:v]fps=8[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle" \
    -loop 0 "$OUT/render-lab-sponza.gif"

# --- sand: title screen and the volcano clip ------------------------------
S=$WORK/sand
sh launcher/main/apps/sand/tools/sand_menu_render_host.sh -o "$S" > "$WORK/sand_menu.log"
bmp_to_png "$S/title-landscape.bmp" "$OUT/sand-menu.png"
"$PYTHON" launcher/main/apps/sand/tools/make_volcano_clip.py -o "$OUT/sand-simulation.gif" > /dev/null

if [ "$CHECK" = 0 ]; then
    echo "wrote $(ls "$IMAGES" | wc -l | tr -d ' ') images to $IMAGES"
    exit 0
fi

status=0
for made in "$OUT"/*; do
    name=$(basename "$made")
    if [ ! -f "$IMAGES/$name" ]; then
        echo "changed $name: new image"
        status=1
    elif result=$("$PYTHON" "$TOOLS_DIR/compare_images.py" "$IMAGES/$name" "$made"); then
        echo "same $name"
    else
        echo "changed $name: ${result#different: }"
        status=1
    fi
done
for kept in "$IMAGES"/*; do
    if [ ! -f "$OUT/$(basename "$kept")" ]; then
        echo "orphan $(basename "$kept"): nothing in this script makes it"
        status=1
    fi
done
exit $status
