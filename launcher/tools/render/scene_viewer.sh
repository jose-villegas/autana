#!/bin/sh
# Builds one scene file's pack through the shared harness, then runs its viewer;
# --build-only -o DIR prints the built host instead; --deps prints what
# the host is compiled from (render_scene.sh).
#
# --compare REV|DIR renders the whole camera path as one A | B video: A is this
# checkout, B the scene's pack built at a git revision or in an existing
# checkout (. is this one), drawn by this checkout's viewer. --b-args "ARGS"
# gives B other renderer arguments than A's. -o names the video (default:
# the results tree's scene_viewer/compare/<scene>.mp4). --frame N renders frame N
# of the same path to a PNG, both sides' with --compare (-o names A's; B's
# adds -b). scene_path.py says how the path is timed.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd -P)
if [ $# -eq 0 ]; then
    echo "usage: $0 SCENE.scene.toml [--build-only -o DIR | --deps] [--object NAME ...] [--camera NAME] [--frames N --dt MS] [--view shaded|depth|tiles|motion|meshlets|cull] [--counts out.csv] [--poses out.csv] [--size WxH] [--present nearest|scanline|double] [--small-fill CEILING,GATE,STYLE] [--set owner.name=value ...] [--tunables] [--quarter 0|1] [--panel] [-o out.bmp] [--video out.avi]" >&2
    echo "       $0 SCENE.scene.toml [--compare REV|DIR [--b-args ARGS]] [--frame N] [-o out.mp4|out.png] [renderer arguments]" >&2
    exit 2
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
build_dir=$SCRIPT_DIR/../results/render/scene_viewer
compare=
b_args=
b_given=0
frame=
path_args=
render_args=
output=
while [ $# -gt 0 ]; do
    case "$1" in
        --build-only) build_only=1; shift ;;
        --deps) deps=1; shift ;;
        -o|--compare|--b-args|--frame|--frames|--dt)
            [ $# -ge 2 ] || { echo "$1 needs a value" >&2; exit 2; }
            case "$1" in
                -o) output=$2 ;;
                --compare) compare=$2 ;;
                --b-args) b_args=$2; b_given=1 ;;
                --frame) frame=$2 ;;
                *) path_args="${path_args}${path_args:+
}$1
$2" ;;
            esac
            shift 2 ;;
        *) render_args="${render_args}${render_args:+
}$1"; shift ;;
    esac
done
if [ "$b_given" = 1 ] && [ -z "$compare" ]; then
    echo "scene_viewer: --b-args needs --compare REV|DIR (. for this checkout)" >&2
    exit 2
fi
if [ -z "$compare$frame" ]; then
    # Neither renders the path itself: --frames and --dt are the viewer's own.
    render_args="${render_args}${render_args:+${path_args:+
}}${path_args}"
fi
if [ -n "$output" ]; then
    if [ "$build_only" = 1 ]; then
        build_dir=$output
    elif [ -z "$compare$frame" ]; then
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
. "$SCRIPT_DIR/../../../scripts/lib/native_path.sh"
AUTANA_ASSET_DIR=$(to_native "$(dirname "$binary")/assets")
export AUTANA_ASSET_DIR
id=$(basename "$scene_file" .scene.toml)

if [ -n "$compare$frame" ]; then
    . "$SCRIPT_DIR/../../../scripts/lib/python.sh"
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    trap 'exit 130' INT TERM
    a_lines="--scene
$id${render_args:+
}$render_args"
    set -- --side "$(printf 'this checkout\n%s' "$render_args" | tr '\n' ' ')" "$(to_native "$binary")" \
        "$AUTANA_ASSET_DIR" "$a_lines"
    if [ -n "$compare" ]; then
        . "$SCRIPT_DIR/../revision_worktree.sh"
        revision_worktree_setup "$REPO" "$work"
        trap 'revision_worktree_cleanup; rm -rf "$work"' EXIT
        b_root=$(revision_worktree_checkout "$compare" b)
        b_root=$(CDPATH= cd -- "$b_root" && pwd -P)
        b_pack=$AUTANA_ASSET_DIR
        if [ "$b_root" != "$REPO" ]; then
            scene_abs=$(CDPATH= cd -- "$(dirname -- "$scene_file")" && pwd -P)/$(basename -- "$scene_file")
            scene_rel=${scene_abs#"$REPO"/}
            [ "$scene_rel" != "$scene_abs" ] || { echo "scene_viewer: --compare needs a scene file inside $REPO" >&2; exit 2; }
            [ -f "$b_root/$scene_rel" ] || { echo "scene_viewer: $compare has no $scene_rel" >&2; exit 2; }
            echo "scene_viewer: building B's pack in $b_root" >&2
            b_build=$(sh "$b_root/launcher/tools/render/scenes/scene_viewer_render_host.sh" --build-only \
                --asset-file "$b_root/$scene_rel" -o "$work/b_build")
            b_pack=$(to_native "$(dirname "${b_build#built }")/assets")
        fi
        b_lines=$render_args
        if [ "$b_given" = 1 ]; then
            # shellcheck disable=SC2086 # the arguments are words, unglobbed
            b_lines=$(set -f; printf '%s\n' $b_args)
        fi
        set -- "$@" --side "$(printf '%s\n%s' "$compare" "$b_lines" | tr '\n' ' ')" "$(to_native "$binary")" "$b_pack" "--scene
$id${b_lines:+
}$b_lines"
    fi
    IFS='
'
    # shellcheck disable=SC2086 # one argument per line
    set -- "$@" $path_args
    unset IFS
    if [ -n "$frame" ]; then
        set -- frame "$frame" --out "$(to_native "${output:-$build_dir/frames/$id-f$frame.png}")" "$@"
    else
        set -- video --out "$(to_native "${output:-$build_dir/compare/$id.mp4}")" "$@"
    fi
    "$(find_python PIL numpy)" "$(to_native "$SCRIPT_DIR/scene_path.py")" "$@" --work "$(to_native "$work")"
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
