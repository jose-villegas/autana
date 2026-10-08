#!/bin/sh
# Builds one scene file's pack through the shared harness, then runs its viewer;
# --build-only -o DIR prints the built host instead.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ $# -eq 0 ]; then
    echo "usage: $0 SCENE.scene.toml [--build-only -o DIR] [--object NAME ...] [--camera NAME] [--frames N --dt MS] [--view shaded|depth|tiles|motion] [--size WxH] [--quarter 0|1] [--panel] [-o out.bmp] [--video out.avi]" >&2
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
build_dir=$SCRIPT_DIR/../results/render/scene_viewer
render_args=
output=
while [ $# -gt 0 ]; do
    case "$1" in
        --build-only) build_only=1; shift ;;
        -o)
            [ $# -ge 2 ] || { echo "-o needs a path" >&2; exit 2; }
            output=$2; shift 2 ;;
        *) render_args="${render_args}${render_args:+
}$1"; shift ;;
    esac
done
if [ -n "$output" ]; then
    if [ "$build_only" = 1 ]; then
        build_dir=$output
    else
        render_args="${render_args}${render_args:+
}-o
$output"
    fi
fi
set -- --build-only --asset-file "$scene_file" -o "$build_dir"
IFS='
'
build=$(sh "$SCRIPT_DIR/scenes/scene_viewer_render_host.sh" "$@")
if [ "$build_only" = 1 ]; then
    printf '%s\n' "$build"
    exit 0
fi
binary=${build#built }
. "$SCRIPT_DIR/../../../scripts/lib/native_path.sh"
AUTANA_ASSET_DIR=$(to_native "$(dirname "$binary")/assets")
export AUTANA_ASSET_DIR
id=$(basename "$scene_file" .scene.toml)
set --
for argument in $render_args; do
    set -- "$@" "$argument"
done
exec "$binary" --scene "$id" "$@"
