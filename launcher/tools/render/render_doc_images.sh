#!/bin/sh
#
# Regenerate host images and measured Markdown tables from the source.
# GPU and board stages share the block writer and use separate output folders.
#
#   ./launcher/tools/render/render_doc_images.sh            # rewrite images and tables
#   ./launcher/tools/render/render_doc_images.sh --check    # only report which changed
#   ./launcher/tools/render/render_doc_images.sh --orphans  # only report images no script names
#   ./launcher/tools/render/render_doc_images.sh --stage gpu [--smoke|--check]
#   ./launcher/tools/render/render_doc_images.sh --stage board --capture PATH [--check]
#
# Needs host C and C++ compilers, Python with Pillow and numpy, ffmpeg 5.1
# or newer, and the initialized meshoptimizer submodule for scratch bakes.
# Runs in Git Bash on Windows and on Linux.
#
# This makes the launcher and UI toolkit images itself and then runs every
# launcher/main/apps/*/tools/doc_images.sh, which makes that app's images into
# the out tree it is given. It also runs doc_images_demo.sh for the demo scene.
# Everything is rendered into
# launcher/tools/results/doc_images/out/cpu/, laid out like docs/images/, first.
#
# --check compares each result with the committed image by decoded pixels
# (compare_images.py), never by bytes: two encoder versions write different
# files for one picture. It prints "same <image>" or "changed <image>: <how>"
# per image and "changed <doc>#<name>" per changed table, and exits 1 when
# any changed. An image in the folder that no script made is reported as
# "orphan" and also exits 1. Exit 2 means the images or tables could not be
# made or compared; failed commands and render-log tails are printed.
#
# doc_images_demo.sh and the dynamic-resolution report write out/tables/NAME.md.
# generated_blocks.py rewrites the matching named blocks; --check compares
# them without writing.
#
# The Cornell box is traced in float, and GIF palettes depend on the ffmpeg
# version, so a --check on another OS or with another ffmpeg may report them
# changed; CI renders on Linux and is the authority.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$TOOLS_DIR/../../.." && pwd)
cd "$ROOT"

. scripts/lib/run.sh

DEMO_SCENE=launcher/demo/sponza/sponza.scene.toml
DEMO_OBJECT=atrium

if [ "${1:-}" = --stage ]; then
    if [ "${2:-}" = gpu ]; then
        shift 2
        exec sh "$TOOLS_DIR/run_doc_gpu.sh" --scene "$DEMO_SCENE" --object "$DEMO_OBJECT" "$@"
    fi
    . "$ROOT/scripts/lib/python.sh"
    PYTHON=$(find_python numpy scipy) || exit 2
    exec "$PYTHON" "$TOOLS_DIR/doc_stages.py" --scene "$DEMO_SCENE" --object "$DEMO_OBJECT" "$@"
fi

IMAGES=docs/images
RESULTS=launcher/tools/results/doc_images
WORK=$RESULTS/work/cpu
OUT=$RESULTS/out/cpu

# Any failure before the compare loop is "could not render", exit 2. An
# `|| exit 2` per command would not do: set -e is off inside AND-OR lists.
comparing=0
finish() {
    code=$?
    if [ "$comparing" = 0 ] && [ "$code" != 0 ]; then
        if [ -d "$WORK" ]; then
            find "$WORK" -type f -name '*.log' -exec sh -c '
                for log do
                    echo "--- $log" >&2
                    tail -n 20 "$log" >&2
                done
            ' sh {} +
        fi
        exit 2
    fi
}
trap finish 0

# The one orphan report: --check prints it for an image no run made, --orphans
# for an image no script names.
orphan() {
    echo "orphan $1: no script makes it"
}

CHECK=0
case "${1:-}" in
    "") ;;
    --check) CHECK=1 ;;
    --orphans)
        # Cheap enough for a pull request: nothing is rendered. An image is
        # claimed when its name up to the first dot appears in the script that
        # makes the launcher images, the scene scripts, or an app's doc_images.sh.
        comparing=1
        status=0
        for image in $(cd "$IMAGES" && find . -type f | sed 's|^\./||' | LC_ALL=C sort); do
            case "$image" in render/gpu/*) continue ;; esac
            stem=$(basename "$image")
            stem=${stem%%.*}
            if ! grep -qF -- "$stem" "$TOOLS_DIR/render_doc_images.sh" "$TOOLS_DIR"/scenes/*.sh "$TOOLS_DIR/doc_images_demo.sh" "$TOOLS_DIR/doc_import_examples.py" \
                launcher/main/apps/*/tools/doc_images.sh; then
                claimed=0
                for scene in launcher/demo/*/*.scene.toml; do
                    id=$(basename "$scene" .scene.toml)
                    case "$stem" in
                        "$id"-*)
                            template='${ID}'${stem#"$id"}
                            if grep -qF -- "$template" "$TOOLS_DIR/doc_images_demo.sh"; then
                                claimed=1
                                break
                            fi ;;
                    esac
                done
                if [ "$claimed" = 0 ]; then
                    orphan "$image"
                    status=1
                fi
            fi
        done
        exit $status
        ;;
    *) echo "usage: $0 [--check|--orphans]" >&2; exit 2 ;;
esac

# Windows has a python3 launcher stub that is not Python; ask for Pillow and numpy.
# shellcheck source=../../../scripts/lib/python.sh
. "$(dirname "$0")/../../../scripts/lib/python.sh"
PYTHON=$(find_python PIL numpy) || exit 2
export PYTHON
if ! command -v ffmpeg > /dev/null 2>&1; then
    echo "ffmpeg not found." >&2
    exit 2
fi

run rm -rf "$WORK" "$OUT"
run mkdir -p "$WORK" "$OUT/overview" "$OUT/ui"

# The launcher lists the release build's apps, read from the tree.
L=$WORK/launcher_home
run sh launcher/tools/render/scenes/launcher_home_render_host.sh -o "$L" > "$WORK/launcher_home.log"
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
run "$L/launcher_home_render" --quarter 1 "$@" -o "$L/release.bmp" 2> "$WORK/launcher_home_png.log"
run "$PYTHON" -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' \
    "$L/release.bmp" "$OUT/overview/launcher-home.png"
# The board rocking either way.
run "$L/launcher_home_render" --quarter 1 --tilt-sweep "$@" --frames 250 --dt 16 \
    -o "$L/sweep.bmp" --video "$L/sweep.avi" 2> "$WORK/launcher_home_sweep.log"
run ffmpeg -hide_banner -loglevel error -y -i "$L/sweep.avi" \
    -vf "fps=15,palettegen=stats_mode=diff" "$L/sweep-palette.png"
run ffmpeg -hide_banner -loglevel error -y -i "$L/sweep.avi" -i "$L/sweep-palette.png" \
    -filter_complex "[0:v]fps=15[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle" \
    -loop 0 "$OUT/overview/launcher-home.gif"

# The UI toolkit's gallery views.
U=$WORK/ui_widgets
run sh launcher/tools/render/scenes/ui_widgets_render_host.sh -o "$U" > "$WORK/ui_widgets.log"
for bmp in "$U"/*.bmp; do
    run "$PYTHON" -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' \
        "$bmp" "$OUT/ui/$(basename "$bmp" .bmp).png"
done

# Each app that has doc images makes them.
for script in launcher/main/apps/*/tools/doc_images.sh; do
    [ -f "$script" ] || continue
    app=$(basename "$(dirname "$(dirname "$script")")")
    run bash "$script" "$OUT" "$WORK/$app"
done

run bash "$TOOLS_DIR/doc_images_demo.sh" "$OUT" "$WORK/demo" "$DEMO_SCENE" "$DEMO_OBJECT"

# Dynamic resolution's tables and chart, from the board capture and the
# reference scores kept beside its page.
run mkdir -p "$OUT/render" "$OUT/tables"
set --
for csv in docs/render/data/dynamic-resolution-quality-*.csv; do
    [ -f "$csv" ] || continue
    camera=${csv##*/dynamic-resolution-quality-}
    camera=${camera%.csv}
    set -- "$@" --quality "$camera=$csv"
done
run "$PYTHON" launcher/tools/r3d/dynres_report.py docs/render/data/dynamic-resolution-board.log \
    "$@" --present docs/render/data/pipeline-present-board.log --tables "$OUT/tables" \
    --chart "$OUT/render/dynamic-resolution-flight.png" > "$WORK/dynres.log"

table_status=0
if [ "$CHECK" = 1 ]; then
    "$PYTHON" "$TOOLS_DIR/generated_blocks.py" --tables "$OUT/tables" --check || table_status=$?
    [ "$table_status" -le 1 ] || exit 2
else
    "$PYTHON" "$TOOLS_DIR/generated_blocks.py" --tables "$OUT/tables"
fi

if [ "$CHECK" = 0 ]; then
    for dir in "$OUT"/*; do
        [ "$(basename "$dir")" = tables ] && continue
        run cp -R "$dir" "$IMAGES"/
    done
    echo "wrote $(find "$OUT" -type f ! -path "*/tables/*" | wc -l | tr -d ' ') images to $IMAGES"
    exit 0
fi

comparing=1
status=$table_status
made_list=$(cd "$OUT" && find . -type f ! -path "./tables/*" | sed 's|^\./||' | LC_ALL=C sort)
kept_list=$(cd "$IMAGES" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
for name in $made_list; do
    if [ ! -f "$IMAGES/$name" ]; then
        echo "changed $name: new image"
        status=1
        continue
    fi
    rc=0
    result=$("$PYTHON" "$TOOLS_DIR/compare_images.py" "$IMAGES/$name" "$OUT/$name") || rc=$?
    case $rc in
        0) echo "same $name" ;;
        1) echo "changed $name: ${result#different: }"; status=1 ;;
        *) echo "unreadable $name: $result" >&2; exit 2 ;;
    esac
done
for name in $kept_list; do
    case "$name" in render/gpu/*) continue ;; esac
    if [ ! -f "$OUT/$name" ]; then
        orphan "$name"
        status=1
    fi
done
exit $status
