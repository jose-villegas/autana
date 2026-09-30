#!/bin/sh
#
# How does one revision render against another? Builds a scene's host
# renderer at each of two revisions, renders the same views, and writes a
# sheet per group of views - A | B | amplified difference - plus a summary.
#
#   ./launcher/tools/render/render_compare.sh --script <host-render-script> \
#       [-o <dir>] [--clear RRGGBB] [--gain N] <A> <B> \
#       [--sheet <name>] --render <label> "<renderer arguments>" ...
#
# <A> and <B> are git revisions, each checked out in a temporary worktree
# that is removed afterwards, or existing directories used as they are.
# --script is the scene's own host-render script (a render_scene.sh caller),
# relative to the checkout root, so the tool names no app and no scene; the
# renderer arguments are that scene's own (--scene, --frames, ...). --sheet
# starts a new sheet; renders before any --sheet go on one called "compare".
# --clear is the colour the scene clears to, for marking holes; see
# render_compare.py, which also writes <dir>/summary.txt. -o defaults to
# a fresh temporary directory.
#
# POSIX sh, like the rest of this directory.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$TOOLS_DIR/../../.." && pwd)

usage() {
    sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//' >&2
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
gain=8
rev_a=""
rev_b=""
sheet="compare"
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
trap cleanup EXIT INT TERM

while [ $# -gt 0 ]; do
    case "$1" in
        --script) script=${2:?--script needs a path}; shift 2 ;;
        -o) out=${2:?-o needs a directory}; shift 2 ;;
        --clear) clear_arg=${2:?--clear needs RRGGBB}; shift 2 ;;
        --gain) gain=${2:?--gain needs a number}; shift 2 ;;
        --sheet) sheet=${2:?--sheet needs a name}; shift 2 ;;
        --render)
            [ $# -ge 3 ] || usage
            printf '%s\t%s\t%s\n' "$sheet" "$2" "$3" >> "$renders"
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
[ -n "$script" ] && [ -n "$rev_b" ] && [ -s "$renders" ] || usage
[ -n "$out" ] || out=$(mktemp -d)
mkdir -p "$out"

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

# Builds the scene renderer in a checkout and renders every view into $out/<side>.
render_side() {
    side=$1
    rev=$2
    tree=$(checkout "$rev" "$side") || exit 1
    [ -f "$tree/$script" ] || { echo "$rev has no $script" >&2; exit 1; }
    mkdir -p "$out/$side" "$work/build_$side"
    built=$(cd "$tree" && sh "$script" -o "$work/build_$side" --build-only 2> /dev/null | sed -n 's/^built //p') || built=""
    if [ -z "$built" ]; then
        # A revision from before --build-only: its script renders every
        # declared scene too, and leaves the renderer beside them.
        (cd "$tree" && sh "$script" -o "$work/build_$side" > "$work/build.log" 2>&1) || {
            tail -5 "$work/build.log" >&2
            echo "build failed at $rev" >&2
            exit 1
        }
        built=$(ls "$work/build_$side"/*_render "$work/build_$side"/*_render.exe 2> /dev/null | head -1)
    fi
    [ -n "$built" ] || { echo "no renderer built for $rev" >&2; exit 1; }
    while IFS="$(printf '\t')" read -r _sheet label args; do
        # shellcheck disable=SC2086
        (cd "$tree" && "$built" $args -o "$out/$side/$label.bmp") > "$work/render.log" 2>&1 || {
            cat "$work/render.log" >&2
            echo "render $label failed at $rev" >&2
            exit 1
        }
    done < "$renders"
    echo "$side: $rev" >> "$out/summary.txt"
}

: > "$out/summary.txt"
render_side a "$rev_a"
render_side b "$rev_b"

for name in $(cut -f1 "$renders" | awk '!seen[$0]++'); do
    set --
    while IFS="$(printf '\t')" read -r sheet_name label _args; do
        [ "$sheet_name" = "$name" ] || continue
        set -- "$@" --row "$label" "$(to_native "$out/a/$label.bmp")" "$(to_native "$out/b/$label.bmp")"
    done < "$renders"
    if [ -n "$clear_arg" ]; then
        set -- --clear "$clear_arg" "$@"
    fi
    echo "== $name" >> "$out/summary.txt"
    "$PYTHON" "$(to_native "$TOOLS_DIR/render_compare.py")" --gain "$gain" \
        --out "$(to_native "$out/$name.png")" --summary "$(to_native "$out/summary.txt")" "$@"
    echo "sheet $out/$name.png"
done
echo "summary $out/summary.txt"
