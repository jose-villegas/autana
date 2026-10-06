#!/bin/sh
#
# How does one revision render against another? Runs a scene's host-render
# script at each of two revisions and compares what it wrote: one sheet of
# A | B | amplified difference rows, and a summary.
#
#   ./launcher/tools/render/render_compare.sh --script <host-render-script> \
#       [-o <dir>] [--clear RRGGBB] [--video [--fps N]] [--crops N] <A> <B> \
#       [--render <label> "<renderer arguments>" ...]
#
# <A> and <B> are git revisions, each checked out in a temporary worktree
# that is removed afterwards, or existing directories used as they are.
# --script is the scene's own host-render script (a render_scene.sh caller),
# relative to the checkout root, so the tool names no app and no scene.
# By default it runs that script at both revisions and compares the images
# of the same name. --render replaces that with ad-hoc renders: it builds the
# scene's renderer only (the script's --build-only) and runs it with the
# arguments given, which are that scene's own (--scene, --frames, ...).
# --clear is the colour the scene clears to, for marking holes. The sheet is
# <dir>/compare.png; render_compare.py also writes <dir>/summary.txt. -o
# defaults to a fresh temporary directory.
#
# --video also records every render's frames on both sides, so with --render
# put --frames and --dt in the renderer arguments, and writes <dir>/<label>.mp4
# (A | B | heatmap per frame, labelled with the revisions' short hashes) and
# <label>.frames.csv (changed share, mean difference and holes per frame).
# --fps sets the playback rate; the default is the recorded one, 1000/dt.
# Needs ffmpeg.
#
# --crops N also writes compare.crops.png and <label>.crops.png: the N places
# the two revisions differ most, A above B, enlarged. A video's come from its
# two worst frames. Nothing is written where the two do not differ.
#
# --reference SCENE.scene.toml --poses FILE --render LABEL "ARGS" compares
# <A> against the scene's source reference instead of a <B>: the reference
# frames come from r3d/reference_render.py at the poses in FILE, the camera
# path sampled at the renderer's --dt (tools/anim/track_host.py --every DT),
# and are cached under r3d/.cache/reference by a hash of the scene, its import
# files (which pin the source model's sha256), the r3d sources, the poses and N
# (--samples N is the supersampling, default 4). It
# implies --video: <label>.mp4 is reference | render | dE heatmap | edge pixels
# per frame, at --fps 30 unless 40, 60 or 80 is given, and summary.txt gets
# mean and 95th-percentile dE and SSIM per frame. A render's first frame is
# one --dt in, so the reference skips the pose at time zero. --start N scores
# from render frame N, for a poses file that begins at pose N. --r3d-python names
# the interpreter with the r3d requirements; the default is r3d/.cache/venv's.
#
# --reference-frames with --reference and --poses alone makes (or finds) that
# cache and prints its directory, for a caller that scores the frames itself.
#
# POSIX sh, like the rest of this directory.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$TOOLS_DIR/../../.." && pwd)

usage() {
    sed -n '3,49p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

# shellcheck source=../../../scripts/lib/python.sh
. "$TOOLS_DIR/../../../scripts/lib/python.sh"

PYTHON=$(find_python PIL numpy) || exit 1

. "$TOOLS_DIR/../../../scripts/lib/native_path.sh"

script=""
out=""
clear_arg=""
video=0
fps=""
crops=0
reference=""
poses=""
samples=4
first=0
frames_only=0
r3d_python=""
rev_a=""
rev_b=""
work=$(mktemp -d)
renders="$work/renders.tsv"
: > "$renders"
# shellcheck source=../revision_worktree.sh
. "$TOOLS_DIR/../revision_worktree.sh"
revision_worktree_setup "$REPO_DIR" "$work"

cleanup() {
    revision_worktree_cleanup
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

while [ $# -gt 0 ]; do
    case "$1" in
        --script) script=${2:?--script needs a path}; shift 2 ;;
        -o) out=${2:?-o needs a directory}; shift 2 ;;
        --clear) clear_arg=${2:?--clear needs RRGGBB}; shift 2 ;;
        --video) video=1; shift ;;
        --crops) crops=${2:?--crops needs a number}; shift 2 ;;
        --reference) reference=${2:?--reference needs a scene file}; video=1; shift 2 ;;
        --poses) poses=${2:?--poses needs a file}; shift 2 ;;
        --start) first=${2:?--start needs a frame number}; shift 2 ;;
        --reference-frames) frames_only=1; shift ;;
        --samples) samples=${2:?--samples needs a number}; shift 2 ;;
        --r3d-python) r3d_python=${2:?--r3d-python needs a path}; shift 2 ;;
        --fps) fps=${2:?--fps needs a number}; shift 2 ;;
        --render)
            [ $# -ge 3 ] || usage
            printf '%s\t%s\n' "$2" "$3" >> "$renders"
            shift 3
            ;;
        -h | --help) usage ;;
        -*) echo "unknown option $1" >&2; usage ;;
        *)
            if [ -z "$rev_a" ]; then rev_a=$1
            elif [ -z "$rev_b" ]; then rev_b=$1
            else usage
            fi
            shift
            ;;
    esac
done
if [ "$frames_only" = 1 ]; then
    [ -n "$reference" ] && [ -n "$poses" ] || usage
    out=$work
elif [ -n "$reference" ]; then
    [ -n "$script" ] && [ -n "$rev_a" ] && [ -n "$poses" ] && [ -z "$rev_b" ] && [ -s "$renders" ] || usage
    [ -n "$fps" ] || fps=30
    case "$fps" in
        30|40|60|80) ;;
        *) echo "--fps with --reference is 30, 40, 60 or 80, not $fps." >&2; exit 2 ;;
    esac
else
    [ -n "$script" ] && [ -n "$rev_b" ] || usage
fi
if [ "$video" = 1 ] && ! command -v ffmpeg > /dev/null 2>&1; then
    echo "--video needs ffmpeg on PATH." >&2
    exit 1
fi
[ -n "$out" ] || out=$(mktemp -d)
mkdir -p "$out"

# What names a side: a revision's short hash, a directory's name.
short_name() {
    if [ -d "$1" ]; then
        basename "$1"
    else
        git -C "$REPO_DIR" rev-parse --short=8 "$1^{commit}"
    fi
}

# Prints the checkout directory for a revision or directory.
checkout() {
    revision_worktree_checkout "$1" "$2"
}

# Runs the scene script at a checkout, or builds its renderer and runs that
# on the --render list, leaving every image in $out/<side>.
render_side() {
    side=$1
    rev=$2
    tree=$(checkout "$rev" "$side") || exit 1
    [ -f "$tree/$script" ] || { echo "$rev has no $script" >&2; exit 1; }
    mkdir -p "$out/$side"
    video_flag=""
    if [ "$video" = 1 ]; then video_flag="--video"; fi
    if [ ! -s "$renders" ]; then
        # shellcheck disable=SC2086
        (cd "$tree" && sh "$script" -o "$out/$side" $video_flag) > "$work/run.log" 2>&1 || {
            tail -20 "$work/run.log" >&2
            echo "$script failed at $rev" >&2
            exit 1
        }
        return
    fi
    if (cd "$tree" && sh "$script" -o "$out/$side" --build-only) > "$work/build.log" 2>&1; then
        built=$(sed -n 's/^built //p' "$work/build.log")
    else
        # A revision from before --build-only rejects it; its script builds
        # and renders every declared scene, leaving the one renderer here.
        (cd "$tree" && sh "$script" -o "$out/$side") > "$work/build.log" 2>&1 || {
            tail -20 "$work/build.log" >&2
            echo "build failed at $rev" >&2
            exit 1
        }
        built=$(find "$out/$side" -name '*_render' -o -name '*_render.exe' | head -1)
    fi
    [ -n "$built" ] || { echo "no renderer built for $rev" >&2; exit 1; }
    while IFS="$(printf '\t')" read -r label args; do
        video_args=""
        if [ "$video" = 1 ]; then video_args="--video $out/$side/$label.avi"; fi
        # The arguments are split on spaces and must not glob.
        set -f
        # shellcheck disable=SC2086
        (cd "$tree" && "$built" $args -o "$out/$side/$label.bmp" $video_args) > "$work/render.log" 2>&1 || {
            set +f
            cat "$work/render.log" >&2
            echo "render $label failed at $rev" >&2
            exit 1
        }
        set +f
    done < "$renders"
}

# The reference frames for $poses, made once per scene, poses and sampling.
reference_frames() {
    scene_dir=$(dirname "$reference")
    key=$("$PYTHON" -c 'import hashlib, pathlib, sys
digest = hashlib.sha256()
for path in sys.argv[1:-1]:
    digest.update(pathlib.Path(path).read_bytes())
digest.update(sys.argv[-1].encode())
print(digest.hexdigest()[:16])' "$(to_native "$reference")" "$(to_native "$poses")" \
        $(for f in "$scene_dir"/*.import.toml "$TOOLS_DIR/../r3d"/*.py "$TOOLS_DIR/render_compare.py"; do to_native "$f"; done)         "$samples")
    cache="$REPO_DIR/launcher/tools/r3d/.cache/reference/$key"
    if [ ! -f "$cache/done" ]; then
        [ -n "$r3d_python" ] || r3d_python=$(find_r3d_python "$REPO_DIR") || exit 1
        mkdir -p "$cache"
        "$r3d_python" "$(to_native "$TOOLS_DIR/../r3d/reference_render.py")" "$(to_native "$reference")" \
        --poses "$(to_native "$poses")" --skip 1 --out "$(to_native "$cache")" --samples "$samples" >&2 || exit 1
        : > "$cache/done"
    fi
    echo "$cache"
}

if [ "$frames_only" = 1 ]; then
    reference_frames
    exit 0
fi

label_a=$(short_name "$rev_a")
label_b=""
[ -n "$rev_b" ] && label_b=$(short_name "$rev_b")
printf 'a: %s\nb: %s\n' "$label_a" "${label_b:-the source reference}" > "$out/summary.txt"
render_side a "$rev_a"
if [ -n "$reference" ]; then
    cache=$(reference_frames)
    for avi in "$out/a"/*.avi; do
        [ -f "$avi" ] || continue
        label=$(basename "$avi" .avi)
        echo "== $label reference" >> "$out/summary.txt"
        "$PYTHON" "$(to_native "$TOOLS_DIR/render_compare.py")" --out "$(to_native "$out/$label.unused.png")" \
        --reference-video "$(to_native "$avi")" "$(to_native "$cache")" \
        --label-a "$label_a" --reference-mp4 "$(to_native "$out/$label.mp4")" --reference-first "$first" ${fps:+--fps "$fps"} \
        --summary "$(to_native "$out/summary.txt")"
        echo "video $out/$label.mp4"
    done
    rm -f "$out/a"/*.avi "$out"/*.unused.png
    echo "summary $out/summary.txt"
    exit 0
fi
render_side b "$rev_b"

# What to compare: the --render labels, or else the images both sides wrote.
labels="$work/labels.txt"
if [ -s "$renders" ]; then
    cut -f1 "$renders" > "$labels"
else
    for image in "$out/a"/*.bmp; do
        [ -f "$image" ] || continue
        name=$(basename "$image" .bmp)
        [ -f "$out/b/$name.bmp" ] && echo "$name"
    done > "$labels"
fi
[ -s "$labels" ] || { echo "the two revisions wrote no image of the same name" >&2; exit 1; }

set --
if [ -n "$clear_arg" ]; then set -- --clear "$clear_arg"; fi
if [ "$crops" -gt 0 ]; then set -- "$@" --crops "$crops"; fi
while IFS= read -r label; do
    set -- "$@" --row "$label" "$(to_native "$out/a/$label.bmp")" "$(to_native "$out/b/$label.bmp")"
done < "$labels"
"$PYTHON" "$(to_native "$TOOLS_DIR/render_compare.py")" \
    --out "$(to_native "$out/compare.png")" --summary "$(to_native "$out/summary.txt")" \
    --label-a "$label_a" --label-b "$label_b" "$@"
echo "sheet $out/compare.png"

if [ "$video" = 1 ]; then
    while IFS= read -r label; do
        [ -f "$out/a/$label.avi" ] && [ -f "$out/b/$label.avi" ] || continue
        set --
        if [ -n "$clear_arg" ]; then set -- --clear "$clear_arg"; fi
        if [ -n "$fps" ]; then set -- "$@" --fps "$fps"; fi
        if [ "$crops" -gt 0 ]; then set -- "$@" --crops "$crops"; fi
        echo "== $label video" >> "$out/summary.txt"
        "$PYTHON" "$(to_native "$TOOLS_DIR/render_compare.py")" "$@" \
            --video "$(to_native "$out/a/$label.avi")" "$(to_native "$out/b/$label.avi")" \
            --label-a "$label_a" --label-b "$label_b" \
            --out "$(to_native "$out/$label.mp4")" --csv "$(to_native "$out/$label.frames.csv")" \
            --summary "$(to_native "$out/summary.txt")"
        echo "video $out/$label.mp4"
    done < "$labels"
    rm -f "$out/a"/*.avi "$out/b"/*.avi
fi
echo "summary $out/summary.txt"
