#!/bin/sh
#
# The sand app's images for docs/images/overview/, made by
# launcher/tools/render/render_readme_images.sh.
#
#   readme_images.sh <out-dir> <work-dir>
#
# Run from the repository root with $PYTHON set to a Python that has Pillow;
# ffmpeg must be on PATH. Renderer output goes to logs under <work-dir>.

set -eu

OUT=$1
W=$2
mkdir -p "$W"

sh launcher/main/apps/sand/tools/sand_menu_render_host.sh -o "$W" > "$W/menu.log"
"$PYTHON" -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' \
    "$W/title-landscape.bmp" "$OUT/sand-menu.png"

"$PYTHON" launcher/main/apps/sand/tools/make_volcano_clip.py -o "$OUT/sand-simulation.gif" > "$W/clip.log"
