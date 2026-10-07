#!/bin/sh
# Builds one scene file's pack through the shared harness, then runs its viewer.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ $# -eq 0 ]; then
    echo "usage: $0 SCENE.scene.toml [--object NAME ...] [--camera NAME] [--frames N --dt MS] [--view shaded|depth|tiles|motion] [--size WxH] [--quarter 0|1] [--panel] [-o out.bmp] [--video out.avi]" >&2
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
build=$(sh "$SCRIPT_DIR/scenes/scene_viewer_render_host.sh" --build-only --asset-file "$scene_file")
binary=${build#built }
. "$SCRIPT_DIR/../../../scripts/lib/native_path.sh"
AUTANA_ASSET_DIR=$(to_native "$(dirname "$binary")/assets")
export AUTANA_ASSET_DIR
id=$(basename "$scene_file" .scene.toml)
exec "$binary" --scene "$id" "$@"
