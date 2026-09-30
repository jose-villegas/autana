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
# POSIX sh, like the rest of this directory.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$TOOLS_DIR/../../.." && pwd)

usage() {
    sed -n '3,32p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

if ! PYTHON=$(command -v python3 || command -v python); then
    echo "No Python found; render_compare.py needs one (Pillow and numpy)." >&2
    exit 1
fi

# Git Bash hands this script MSYS paths (/c/...), which the Windows python
# cannot open; cygpath exists only there.
to_native() {
    if command -v cygpath > /dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

script=""
out=""
clear_arg=""
video=0
fps=""
crops=0
rev_a=""
rev_b=""
work=$(mktemp -d)
renders="$work/renders.tsv"
: > "$renders"
created="$work/worktrees.txt"
: > "$created"

cleanup() {
    while IFS= read -r tree; do
        [ -n "$tree" ] || continue
        git -C "$REPO_DIR" worktree remove --force "$tree" > /dev/null 2>&1 || true
    done < "$created"
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
[ -n "$script" ] && [ -n "$rev_b" ] || usage
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
    if [ -d "$1" ]; then
        (CDPATH= cd -- "$1" && pwd)
        return
    fi
    git -C "$REPO_DIR" rev-parse --verify --quiet "$1^{commit}" > /dev/null || {
        echo "not a revision or directory: $1" >&2
        return 1
    }
    tree="$work/tree_$2"
    git -C "$REPO_DIR" worktree add --detach "$tree" "$1" > /dev/null 2>&1 || return 1
    echo "$tree" >> "$created"
    echo "$tree"
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

label_a=$(short_name "$rev_a")
label_b=$(short_name "$rev_b")
printf 'a: %s\nb: %s\n' "$label_a" "$label_b" > "$out/summary.txt"
render_side a "$rev_a"
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
    --out "$(to_native "$out/compare.png")" --summary "$(to_native "$out/summary.txt")" "$@"
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
