#!/bin/sh
# Builds one scene file's pack through the shared harness, then runs its viewer;
# --build-only -o DIR prints the built host instead; --deps prints what
# the host is compiled from (render_scene.sh).
#
# --live renders the whole camera path and opens a window to scrub it
# (scene_scrub.py says how). --b REV|DIR adds a second side: the scene's pack
# built at a git revision, or in an existing checkout, drawn by this
# checkout's viewer, so only the bake differs. --b-args "ARGS" draws the
# second side with other renderer arguments instead of A's (both flags may be
# given). --label-a and --label-b name the sides; --marks DIR is where the
# window's marks go (default: the results tree's scene_viewer/marks).
# --shot FRAME PNG writes the window's picture at FRAME instead of opening it.
#
# --mark FILE [--side a|b] renders a mark's frame again: the scene file and
# that side's arguments come from the mark, and the arguments that follow are
# added (-o out.bmp, --video, ...).
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
. "$SCRIPT_DIR/../../../scripts/lib/python.sh"
. "$SCRIPT_DIR/../../../scripts/lib/native_path.sh"
if [ $# -eq 0 ]; then
    echo "usage: $0 SCENE.scene.toml [--build-only -o DIR | --deps] [--object NAME ...] [--camera NAME] [--frames N --dt MS] [--view shaded|depth|tiles|motion|meshlets|cull] [--counts out.csv] [--poses out.csv] [--size WxH] [--present nearest|scanline|double] [--small-fill CEILING,GATE,STYLE] [--set owner.name=value ...] [--tunables] [--quarter 0|1] [--panel] [-o out.bmp] [--video out.avi]" >&2
    echo "       $0 SCENE.scene.toml --live [--b REV|DIR] [--b-args ARGS] [--label-a TEXT] [--label-b TEXT] [--marks DIR] [--shot FRAME PNG] [renderer arguments]" >&2
    echo "       $0 --mark FILE [--side a|b] [renderer arguments]" >&2
    exit 2
fi
scrub=$SCRIPT_DIR/scene_scrub.py
if [ "$1" = --mark ]; then
    [ $# -ge 2 ] || { echo "--mark needs a file" >&2; exit 2; }
    mark=$2
    shift 2
    side=a
    if [ "${1:-}" = --side ]; then
        [ $# -ge 2 ] || { echo "--side needs a or b" >&2; exit 2; }
        side=$2
        shift 2
    fi
    replay=$("$(find_python)" "$(to_native "$scrub")" replay "$(to_native "$mark")" --repo "$(to_native "$REPO")" --side "$side")
    IFS='
'
    # shellcheck disable=SC2086 # one argument per line
    set -- $replay "$@"
    unset IFS
    first=$1
    shift
    case "$first" in
        /*|?:*) set -- "$first" "$@" ;;
        *) set -- "$REPO/$first" "$@" ;;
    esac
fi
scene_file=$1
shift
case "$scene_file" in
    *.scene.toml) ;;
    *) echo "scene_viewer: expected a .scene.toml file: $scene_file" >&2; exit 2 ;;
esac
if [ ! -f "$scene_file" ]; then
    echo "scene_viewer: no scene file: $scene_file" >&2
    exit 2
fi
build_only=0
deps=0
live=0
build_dir=$SCRIPT_DIR/../results/render/scene_viewer
marks=$build_dir/marks
b_tree=
b_args=
b_given=0
label_a=
label_b=
live_args=
render_args=
output=
while [ $# -gt 0 ]; do
    case "$1" in
        --build-only) build_only=1; shift ;;
        --deps) deps=1; shift ;;
        --live) live=1; shift ;;
        -o)
            [ $# -ge 2 ] || { echo "-o needs a path" >&2; exit 2; }
            output=$2; shift 2 ;;
        --b|--b-args|--label-a|--label-b|--marks|--frames|--dt)
            [ $# -ge 2 ] || { echo "$1 needs a value" >&2; exit 2; }
            case "$1" in
                --b) b_tree=$2 ;;
                --b-args) b_args=$2; b_given=1 ;;
                --label-a) label_a=$2 ;;
                --label-b) label_b=$2 ;;
                --marks) marks=$2 ;;
                # The window renders the path itself: these are its, not the viewer's.
                *) live_args="${live_args}${live_args:+
}$1
$2" ;;
            esac
            shift 2 ;;
        --shot)
            [ $# -ge 3 ] || { echo "--shot needs FRAME PNG" >&2; exit 2; }
            live_args="${live_args}${live_args:+
}--shot
$2
$(to_native "$3")"
            shift 3 ;;
        *) render_args="${render_args}${render_args:+
}$1"; shift ;;
    esac
done
if [ "$live" = 0 ]; then
    if [ -n "$b_tree$label_a$label_b" ] || [ "$b_given" = 1 ] || [ "$marks" != "$build_dir/marks" ] \
        || case "$live_args" in *--shot*) true ;; *) false ;; esac; then
        echo "scene_viewer: --b, --b-args, --label-a, --label-b, --marks and --shot need --live" >&2
        exit 2
    fi
    # Not live: --frames and --dt are the viewer's own.
    render_args="${render_args}${render_args:+${live_args:+
}}${live_args}"
fi
if [ -n "$output" ]; then
    if [ "$build_only" = 1 ]; then
        build_dir=$output
    elif [ "$live" = 1 ]; then
        echo "scene_viewer: --live writes no image; --shot FRAME PNG writes the window's picture" >&2
        exit 2
    else
        render_args="${render_args}${render_args:+
}-o
$output"
    fi
fi
if [ "$deps" = 1 ]; then
    exec sh "$SCRIPT_DIR/scenes/scene_viewer_render_host.sh" --deps --asset-file "$scene_file"
fi
IFS='
'
build=$(sh "$SCRIPT_DIR/scenes/scene_viewer_render_host.sh" --build-only --asset-file "$scene_file" -o "$build_dir")
unset IFS
if [ "$build_only" = 1 ]; then
    printf '%s\n' "$build"
    exit 0
fi
binary=${build#built }
AUTANA_ASSET_DIR=$(to_native "$(dirname "$binary")/assets")
export AUTANA_ASSET_DIR
id=$(basename "$scene_file" .scene.toml)

if [ "$live" = 1 ]; then
    scene_abs=$(CDPATH= cd -- "$(dirname -- "$scene_file")" && pwd)/$(basename -- "$scene_file")
    scene_rel=${scene_abs#"$REPO"/}
    set -- --side "${label_a:-this checkout}" "$(to_native "$REPO")" "$(to_native "$binary")" "$AUTANA_ASSET_DIR" "$render_args"
    if [ -n "$b_tree" ] || [ "$b_given" = 1 ]; then
        b_pack=$AUTANA_ASSET_DIR
        b_root=$REPO
        if [ -n "$b_tree" ]; then
            work=$(mktemp -d)
            . "$SCRIPT_DIR/../revision_worktree.sh"
            revision_worktree_setup "$REPO" "$work"
            trap 'revision_worktree_cleanup; rm -rf "$work"' EXIT
            trap 'exit 130' INT TERM
            [ "$scene_rel" != "$scene_abs" ] || { echo "scene_viewer: --b needs a scene file inside $REPO" >&2; exit 2; }
            b_root=$(revision_worktree_checkout "$b_tree" b)
            [ -f "$b_root/$scene_rel" ] || { echo "scene_viewer: $b_tree has no $scene_rel" >&2; exit 2; }
            echo "scene_viewer: building B's pack in $b_root" >&2
            b_build=$(sh "$b_root/launcher/tools/render/scenes/scene_viewer_render_host.sh" --build-only \
                --asset-file "$b_root/$scene_rel" -o "$work/b_build")
            b_pack=$(to_native "$(dirname "${b_build#built }")/assets")
        fi
        if [ "$b_given" = 1 ]; then
            # shellcheck disable=SC2086 # the arguments are words, unglobbed
            b_lines=$(set -f; printf '%s\n' $b_args)
        else
            b_lines=$render_args
        fi
        set -- "$@" --side "${label_b:-${b_tree:-$b_args}}" "$(to_native "$b_root")" "$(to_native "$binary")" "$b_pack" \
            "$b_lines"
    fi
    live_out=$build_dir/live/$id
    IFS='
'
    # shellcheck disable=SC2086 # one argument per line
    set -- "$@" $live_args
    unset IFS
    "$(find_python)" "$(to_native "$scrub")" live --scene-file "$(to_native "$scene_abs")" --repo "$(to_native "$REPO")" \
        --out "$(to_native "$live_out")" --marks "$(to_native "$marks")" "$@"
    exit
fi

set --
IFS='
'
for argument in $render_args; do
    set -- "$@" "$argument"
done
unset IFS
exec "$binary" --scene "$id" "$@"
