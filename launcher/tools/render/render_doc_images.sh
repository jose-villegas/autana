#!/bin/sh
#
# Regenerate host images and measured Markdown tables from the source.
# GPU and board stages share the block writer and use separate output folders.
#
#   ./launcher/tools/render/render_doc_images.sh            # rewrite images and tables
#   ./launcher/tools/render/render_doc_images.sh --check    # only report which changed
#   ./launcher/tools/render/render_doc_images.sh --audio [--check]
#   ./launcher/tools/render/render_doc_images.sh --orphans  # only report images no script names
#   ./launcher/tools/render/render_doc_images.sh --stage gpu [--smoke|--check]
#   ./launcher/tools/render/render_doc_images.sh --stage gpu --inputs  # print its input fingerprint
#   ./launcher/tools/render/render_doc_images.sh --stage board --capture PATH [--check]
#   ./launcher/tools/render/render_doc_images.sh --stage meshlets [--check]
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
# made or compared; the failed command is printed, with the end of each log
# that records a failure (a command's own error goes to its log).
#
# doc_images_demo.sh and the dynamic-resolution report write out/tables/NAME.md.
# generated_blocks.py rewrites the matching named blocks; --check compares
# them without writing.
#
# GIF palettes depend on the ffmpeg version, so a --check on another OS
# or with another ffmpeg may report them changed; CI renders on Linux and
# is the authority.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$TOOLS_DIR/../../.." && pwd)
cd "$ROOT"

. scripts/lib/run.sh
. launcher/tools/render/gif_from_avi.sh
. launcher/tools/build/packages.sh

app_image_scripts=
for app_dir in $(app_directories launcher); do
    script="${app_dir}tools/doc_images.sh"
    [ -f "$script" ] && app_image_scripts="$app_image_scripts $script"
done

DEMO_SCENE=launcher/demo/sponza/sponza.scene.toml
DEMO_OBJECT=atrium
# The skinned mesh the skeleton and skin figures draw, a cached bake, and its clip.
SKIN_BAKE=capybara.glb
SKIN_CLIP=gallop
# Three of its clips the playback figures blend: first to second to third, then back to the first.
PLAYBACK_CLIPS="walk gallop idle"
# The tree depth whose node boxes the cull figures draw.
CULL_LEVEL=2
# The cull chase GIFs play one path sample per frame at this rate.
CULL_GIF_FPS=8
# Colours in their palette: the camera moves every frame, so fewer keep them small.
CULL_GIF_COLORS=64
# Where Bistro's high view looks down on its street over the ground square: the depth figure's first frame.
DEPTH_FIT_AT_MS=54300

AUDIO_ONLY=0
if [ "${1:-}" = --audio ]; then
    AUDIO_ONLY=1
    shift
fi

if [ "${1:-}" = --stage ]; then
    if [ "${2:-}" = gpu ] && [ "${3:-}" != --inputs ]; then
        shift 2
        exec sh "$TOOLS_DIR/run_doc_gpu.sh" --scene "$DEMO_SCENE" --object "$DEMO_OBJECT" "$@"
    fi
    . "$ROOT/scripts/lib/python.sh"
    # --inputs needs no third-party module (doc_stages.py checks the Python version).
    if [ "${3:-}" = --inputs ]; then
        PYTHON=$(find_python) || exit 2
    else
        PYTHON=$(find_python numpy scipy) || exit 2
    fi
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
        # A command run with its output sent to a log leaves its error and
        # run's "failed (exit N)" line there; print only those logs.
        if [ -d "$WORK" ]; then
            find "$WORK" -type f -name '*.log' -exec grep -l ': failed (exit [0-9]*):' {} + |
                while read -r log; do
                    echo "--- $log" >&2
                    tail -n 40 "$log" >&2
                done
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
        # Cheap enough for a pull request: nothing is rendered. An image is claimed
        # when its name up to the first dot appears in the script that makes the
        # launcher images, the scene scripts, or an app's doc_images.sh; when the
        # demo script spells it ${ID}-... or ends it with a loop variable, which
        # claims any name under that prefix (${ID}-view-$view); or when it is a
        # pair sheet doc_tables.py lists for the demo scene.
        comparing=1
        status=0
        pairs=
        if [ -f "$DEMO_SCENE" ]; then
            . "$ROOT/scripts/lib/python.sh"
            PYTHON=$(find_python) || exit 2
            pairs=$("$PYTHON" "$TOOLS_DIR/doc_tables.py" --pairs "$DEMO_SCENE" "$DEMO_OBJECT") || exit 2
            pairs=$(printf '%s\n' "$pairs" | awk '{print "compare-" $3 "-" $4}')
        fi
        for image in $(cd "$IMAGES" && find . -type f | sed 's|^\./||' | LC_ALL=C sort); do
            case "$image" in render/gpu/*|render/meshlets/*) continue ;; esac
            stem=$(basename "$image")
            stem=${stem%%.*}
            # A face example's renders are named after its file.
            case "$image" in
                ui/*)
                    example=${stem%%-*}
                    if [ -f "$TOOLS_DIR/ui_examples/$example.c" ] || [ -f "$TOOLS_DIR/ui_examples/screens/$example.c" ]; then
                        continue
                    fi ;;
            esac
            if printf '%s\n' "$pairs" | grep -qxF -- "$stem"; then
                continue
            fi
            if ! grep -qF -- "$stem" "$TOOLS_DIR/render_doc_images.sh" "$TOOLS_DIR"/scenes/*.sh "$TOOLS_DIR/doc_images_demo.sh" "$TOOLS_DIR/doc_import_examples.py" \
                $app_image_scripts; then
                claimed=0
                for scene in launcher/demo/*/*.scene.toml; do
                    id=$(basename "$scene" .scene.toml)
                    case "$stem" in
                        "$id"-*)
                            template='${ID}'${stem#"$id"}
                            # A name a loop ends with its variable, as ${ID}-view-$view.
                            if grep -qF -- "$template" "$TOOLS_DIR/doc_images_demo.sh" ||
                                grep -qF -- "${template%-*}-\$" "$TOOLS_DIR/doc_images_demo.sh"; then
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

# Windows has a python3 launcher stub that is not Python; ask for Pillow, numpy and matplotlib.
# shellcheck source=../../../scripts/lib/python.sh
. "$(dirname "$0")/../../../scripts/lib/python.sh"
PYTHON=$(find_python PIL numpy matplotlib) || exit 2
export PYTHON
if [ "$AUDIO_ONLY" = 0 ] && ! command -v ffmpeg > /dev/null 2>&1; then
    echo "ffmpeg not found." >&2
    exit 2
fi

run rm -rf "$WORK" "$OUT"
run mkdir -p "$WORK" "$OUT/overview" "$OUT/ui"

if [ "$AUDIO_ONLY" = 0 ]; then
# The launcher lists the release build's apps, read from the tree.
L=$WORK/launcher_home
run sh launcher/tools/render/scenes/launcher_home_render_host.sh -o "$L" > "$WORK/launcher_home.log"
set --
for dir in $(app_directories launcher); do
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
# Two frames settle the window before the scene's synthetic press.
run "$L/launcher_home_render" --quarter 1 --frames 2 "$@" -o "$L/release.bmp" 2> "$WORK/launcher_home_png.log"
run "$PYTHON" -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' \
    "$L/release.bmp" "$OUT/overview/launcher-home.png"
# The board rocking either way.
run "$L/launcher_home_render" --quarter 1 --tilt-sweep "$@" --frames 250 --dt 16 \
    -o "$L/sweep.bmp" --video "$L/sweep.avi" 2> "$WORK/launcher_home_sweep.log"
gif_from_avi "$L/sweep.avi" "$OUT/overview/launcher-home.gif" "fps=15"

# The boot animation draws in panel orientation; turn it to the landscape the
# other overview clips use.
B=$WORK/boot_anim
run sh launcher/tools/render/scenes/boot_anim_render_host.sh --build-only -o "$B" > "$WORK/boot_anim.log"
boot_ms=$(sed -n 's/^#define BOOT_ANIM_MS \([0-9]*\).*/\1/p' launcher/main/boot/boot_anim_timeline.h)
run "$B/boot_anim_render" 0 "$@" --frames "$(((boot_ms + 15) / 16 + 1))" --dt 16 \
    -o "$B/boot.bmp" --video "$B/boot.avi" 2> "$WORK/boot_anim_video.log"
gif_from_avi "$B/boot.avi" "$OUT/overview/boot-anim.gif" "transpose=2,fps=15" "stats_mode=full"

# UI render tables; source embedding: docs/tools/Generated-Files.md#usage-examples.
U=$WORK/ui_examples
run sh launcher/tools/render/scenes/ui_examples_render_host.sh -o "$U" > "$WORK/ui_examples.log"
run "$PYTHON" "$TOOLS_DIR/ui_examples_doc.py" --renders "$U" --out "$OUT"

# Each app that has doc images makes them.
for script in $app_image_scripts; do
    [ -f "$script" ] || continue
    app=$(basename "$(dirname "$(dirname "$script")")")
    run bash "$script" "$OUT" "$WORK/$app"
done

run bash "$TOOLS_DIR/doc_images_demo.sh" "$OUT" "$WORK/demo" "$DEMO_SCENE" "$DEMO_OBJECT"

# The math, kinematics and physics references' figures, from the libraries' own functions.
# shellcheck source=../build/find_cc.sh
. launcher/tools/build/find_cc.sh
cc=$(find_cc) || exit 2
run "$PYTHON" launcher/tools/input/shake_figures.py --cc "$cc" --out "$OUT/input/shake-level.png" \
    > "$WORK/shake_figures.log"
run "$PYTHON" launcher/tools/input/gravity_figures.py --cc "$cc" --out "$OUT/input" \
    > "$WORK/gravity_figures.log"
# --orphans scans this runner: gravity-trust.png, gravity-bias.gif
run "$PYTHON" launcher/tools/input/motion_axes_figures.py --cc "$cc" --out "$OUT/input" \
    > "$WORK/motion_axes_figures.log"
# --orphans scans this runner: motion-axes.png, motion-rotation.gif
run mkdir -p "$OUT/math"
run "$PYTHON" launcher/tools/math/math_figures.py --cc "$cc" --trs "$OUT/math" \
    --pixel "$OUT/math/camera-to-pixel.png" > "$WORK/math_figures.log"
# --orphans scans this runner: trs-scale.gif, trs-rotate.gif, trs-translate.gif
run "$PYTHON" launcher/tools/math/math_bench_figures.py --out "$OUT/math" > "$WORK/math_bench_figures.log"
# --orphans scans this runner: benchmark-families.png
run "$PYTHON" launcher/tools/math/fast_path_figures.py --out "$OUT/math" > "$WORK/fast_path_figures.log"
# --orphans scans this runner: fast-path-mathf_recip.png, fast-path-mathf_recip_approx.png, fast-path-mathf_rsqrt.png, fast-path-mathf_rsqrt_approx.png, fast-path-mathf_repeat.png, fast-path-mathx_rsqrt_q30.png, fast-path-trig_sin.png
run "$PYTHON" launcher/tools/math/geometry_figures.py --cc "$cc" --out "$OUT/math" > "$WORK/geometry_figures.log"
# --orphans scans this runner: rect-queries.png, circle-rect-queries.png, plane-distance.png, plane-from-points.png, plane-from-rows.png, frustum.png, box3-frustum.gif, box3-sphere-overlap.gif, box3-sphere-segments.png
run "$PYTHON" launcher/tools/math/signal_figures.py --cc "$cc" --out "$OUT/math" > "$WORK/signal_figures.log"
# --orphans scans this runner: goertzel-response.png, goertzel-sweep.gif, pitch-curve.png
run "$PYTHON" launcher/tools/math/stats_figures.py --cc "$cc" --out "$OUT/math" > "$WORK/stats_figures.log"
# --orphans scans this runner: stats-percentile-ranks.png
run "$PYTHON" launcher/tools/math/inverse_depth_figures.py --cc "$cc" --out "$OUT/math" > "$WORK/inverse_depth_figures.log"
# --orphans scans this runner: inverse-depth-step.png
run "$PYTHON" launcher/tools/physics/spring_figures.py --cc "$cc" --out "$OUT/physics" > "$WORK/spring_figures.log"
# --orphans scans this runner: spring-line-wave.gif, spring-line-spacetime.png
run "$PYTHON" launcher/tools/kinematics/orbit_figures.py --cc "$cc" --out "$OUT/kinematics" > "$WORK/orbit_figures.log"
# --orphans scans this runner: orbit-fling.gif, orbit-fling-decay.png, orbit-reset-spring.png, orbit-fit-distance.png

fi

run "$PYTHON" launcher/tools/audio/audio_drc_figures.py --out "$OUT/audio" > "$WORK/audio_drc_figures.log"
run "$PYTHON" launcher/tools/audio/audio_path_figures.py --out "$OUT/audio" > "$WORK/audio_path_figures.log"
run "$PYTHON" launcher/tools/audio/audio_buzz_doc.py --out "$OUT/audio" > "$WORK/audio_buzz_doc.log"
run "$PYTHON" launcher/tools/audio/sound_mix_figures.py --out "$OUT/audio" > "$WORK/sound_mix_figures.log"
run "$PYTHON" launcher/tools/audio/sound_onset_figures.py --out "$OUT/audio" > "$WORK/sound_onset_figures.log"
# --orphans scans this runner: drc-level.png, drc-thd.png, drc-level-dark.png, drc-thd-dark.png,
# audio-frequency.png, audio-volume.png, audio-frequency-dark.png, audio-volume-dark.png,
# sound-onset.png, sound-onset-dark.png,
# sound_mix_capture_v8.png, sound_mix_capture_v32.png, sound_mix_capture_v8-dark.png, sound_mix_capture_v32-dark.png,
# sound_mix_capture_long.png, sound_mix_capture_long-dark.png,
# buzz-shake.png, buzz-score.png, buzz-shake-dark.png, buzz-score-dark.png

if [ "$AUDIO_ONLY" = 0 ]; then

# The skeleton and skin figures: the demo rig over one clip, and a limb through the skinning kernel.
run mkdir -p "$OUT/render" "$OUT/tables"
skin_asset=$("$PYTHON" launcher/tools/bake/bake.py path "$SKIN_BAKE") || exit 2
run "$PYTHON" launcher/tools/r3d/skin/skin_sheet.py "$skin_asset" "$OUT/render" "$OUT/tables" \
    --sheet "$SKIN_CLIP" > "$WORK/skin_sheet.log"
run "$PYTHON" launcher/tools/r3d/skin/skin_figures.py --cc "$cc" --out "$OUT/render/skin-limb.png" \
    > "$WORK/skin_figures.log"
# --orphans scans this runner: skin-skeleton.gif, skin-weights.gif, skin-influences.png, skin-limb.png

# The scene playback figures: those clips through the animation layer's playheads and blends.
run mkdir -p "$OUT/anim"
# shellcheck disable=SC2086 # PLAYBACK_CLIPS is three words
run "$PYTHON" launcher/tools/anim/playback/playback_figures.py "$skin_asset" $PLAYBACK_CLIPS --cc "$cc" \
    --out "$OUT/anim" > "$WORK/playback_figures.log"
# --orphans scans this runner: anim-playhead.png, anim-blend.png, anim-nlerp.png, anim-blend.gif

# The bake method's figures: local occlusion and bounced light on a floor beside a wall, and the scores.
R3D_PYTHON=$(run find_r3d_python "$PWD")
run "$R3D_PYTHON" launcher/tools/r3d/bake_figures/bake_figures.py --out "$OUT/render" > "$WORK/bake_figures.log" 2>&1
# --orphans scans this runner: bake-ao-rays.png, bake-ao-falloff.png, bake-directions.png, bake-bounce-rays.png, bake-bounce-sweep.png, bake-scores.png

# The baked mesh's figures: the demo scene's lite mesh, its clusters and tree, and the clusterizer's build.
tree_mesh=$("$PYTHON" launcher/tools/bake/bake.py path "$(basename "$DEMO_SCENE" .scene.toml).${DEMO_OBJECT}_lite.mesh") || exit 2
run "$R3D_PYTHON" launcher/tools/r3d/bake_figures/mesh_figures.py --mesh "$tree_mesh" --out "$OUT/render" \
    > "$WORK/mesh_figures.log" 2>&1
# --orphans scans this runner: baked-mesh-tree.gif, baked-mesh-ranges.png, meshlet-build.gif

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
[ -f docs/render/data/upscale-filter-quality.csv ] && set -- "$@" --upscale docs/render/data/upscale-filter-quality.csv
run "$PYTHON" launcher/tools/r3d/dynres_report.py docs/render/data/dynamic-resolution-board.log \
    "$@" --tables "$OUT/tables" --chart "$OUT/render/dynamic-resolution-flight.png" --figures "$OUT/render" > "$WORK/dynres.log"
# --orphans scans this runner: dynamic-resolution-stages.png, dynamic-resolution-stages-dark.png, dynamic-resolution-one-core.png, dynamic-resolution-one-core-dark.png, dynamic-resolution-sizes.png, dynamic-resolution-sizes-dark.png, dynamic-resolution-flight-sizes.png, dynamic-resolution-flight-sizes-dark.png
# Surface upscaling's sheet and edge crops, from the frame the upscale
# quality script keeps beside the page (<scene>-<size>-<frame>-*.png).
UPSCALE_CROPS=3
for reference in docs/render/data/upscale-frames/*-reference.png; do
    [ -f "$reference" ] || continue
    stem=${reference%-reference.png}
    frame=${stem##*-}
    size=${stem%-*}
    size=${size##*-}
    run "$PYTHON" launcher/tools/render/render_compare.py --out "$OUT/render/upscale-surface.png"         --reference-stills "frame $frame" "$reference" "$stem-nearest.png" "$stem-surface.png"         --label-a "nearest $size" --label-b "surface $size" --crops "$UPSCALE_CROPS" > "$WORK/upscale_surface.log"
done
# Every mesh scene's frame at each ladder step, and the predictor flown over it.
run "$PYTHON" launcher/tools/r3d/dynres_replay.py docs/render/data/dynamic-resolution-steps-board.log --cc "$cc" \
    --lead bistro-fitted-full --tables "$OUT/tables" --figures "$OUT/render" > "$WORK/dynres_replay.log"

# Frame generation: every mesh scene's warped frames scored on the host.
run sh launcher/tools/r3d/framegen/framegen_docs.sh --out "$OUT/render" "$OUT/tables" > "$WORK/framegen.log" 2>&1
# --orphans scans this runner: framegen-bistro-holes.png, framegen-bistro-path.png, framegen-bistro-path-dark.png, framegen-bistro.gif, framegen-sponza-holes.png, framegen-pricing.png, framegen-pricing-dark.png, framegen-bistro-sheet.png, framegen-bistro-sheet.crops.png

# The frame stages as shipped, what is inside transform and draw, and what
# each debug view adds, from the pipeline captures beside the page.
run "$PYTHON" launcher/tools/render/frame_stages.py --present docs/render/data/pipeline-present-board.log \
    --laps docs/render/data/pipeline-laps-board.log \
    --views 'docs/render/data/pipeline-view-{view}-board.log' --tables "$OUT/tables" \
    --chart "$OUT/render/pipeline-frame-stages.png" > "$WORK/frame_stages.log"

# What the cull keeps along each scene's camera path, from the cull view, and
# what a cluster and a triangle cost, from the board capture beside the page.
for cull in "launcher/demo/bistro/bistro.scene.toml street_fitted_full cull-bistro Bistro" \
    "$DEMO_SCENE atrium_fitted_full cull-sponza Sponza"; do
    # shellcheck disable=SC2086 # four words: scene, object, stem, label
    set -- $cull
    run "$PYTHON" launcher/tools/render/cull_figures.py --scene "$1" --object "$2" --camera camera --stem "$3" \
        --label "$4" --level "$CULL_LEVEL" --out "$OUT/render" --tables "$OUT/tables" --work "$WORK/cull" \
        > "$WORK/$3.log"
    gif_from_avi "$WORK/cull/$3-chase.avi" "$OUT/render/$3-chase.gif" "setpts=N/$CULL_GIF_FPS/TB,fps=$CULL_GIF_FPS" \
        "stats_mode=diff:max_colors=$CULL_GIF_COLORS"
done
# --orphans scans this runner: cull-bistro-sheet.png, cull-bistro-counts.png, cull-bistro-chase.gif,
# cull-sponza-sheet.png, cull-sponza-counts.png, cull-sponza-chase.gif
run "$PYTHON" launcher/tools/render/cull_cost.py docs/render/data/cull-cost-board.log --tables "$OUT/tables" \
    --chart "$OUT/render/cull-cost.png" > "$WORK/cull_cost.log"

# The same frames with depth scaled by near_z and by the per-frame fit, cut
# where the fit stops the most surfaces fighting.
run "$PYTHON" launcher/tools/render/depth_figures.py --scene launcher/demo/bistro/bistro.scene.toml \
    --object street_fitted_full --object ground --camera camera --at-ms "$DEPTH_FIT_AT_MS" \
    --out "$OUT/render/depth-fit-bistro.png" --work "$WORK/depth" > "$WORK/depth.log"

# Each picture's frame cost in each framebuffer layout, from the scene and
# boot animation captures beside the render pages.
run "$PYTHON" launcher/tools/render/layout_costs.py --mesh docs/render/data/layout-mesh-board.log \
    --boot docs/render/data/layout-boot-board.log --tables "$OUT/tables" \
    --chart "$OUT/render/gfx-layout-costs.png" > "$WORK/layout_costs.log"
fi

table_status=0
if [ "$AUDIO_ONLY" = 0 ]; then
if [ "$CHECK" = 1 ]; then
    "$PYTHON" "$TOOLS_DIR/generated_blocks.py" --tables "$OUT/tables" --check || table_status=$?
    [ "$table_status" -le 1 ] || exit 2
else
    "$PYTHON" "$TOOLS_DIR/generated_blocks.py" --tables "$OUT/tables"
fi

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
    if [ "$AUDIO_ONLY" = 1 ]; then
        case "$name" in audio/*) ;; *) continue ;; esac
    fi
    case "$name" in render/gpu/*|render/meshlets/*) continue ;; esac
    if [ ! -f "$OUT/$name" ]; then
        orphan "$name"
        status=1
    fi
done
exit $status
