#!/bin/sh
#
# Build and render every declared scene, and fail if any of them stops
# producing the image it declared.
#
#   ./launcher/tools/render_all_scenes.sh [-o <dir>]
#
# The standing check that the host render harness still works. Scenes are
# found, never listed: every *_render_host.sh under launcher/ is one,
# whether it belongs to the engine (tools/) or to an app (that app's own
# tools/), so adding a scene is one file and deleting an app takes its
# scenes with it.
#
# Self-checking without Python: each scene script compares what the binary
# says it wrote against the size that scene declared, and this fails on the
# first scene that fails. A missing compiler is the one thing that is not a
# failure here, the same way test/check_app_sources.sh treats it.
#
# With -o, each scene writes into its own subdirectory of that directory
# instead of the results/render/ folder beside it.
#
# POSIX sh, like the rest of this directory.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$TOOLS_DIR/.." && pwd)

OUT_ROOT=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT_ROOT="$2"; shift 2 ;;
        *) echo "usage: $0 [-o <dir>]" >&2; exit 2 ;;
    esac
done

# shellcheck source=./find_cc.sh
. "$TOOLS_DIR/find_cc.sh"
if ! find_cc > /dev/null; then
    echo "No C compiler found - skipping the host render scenes." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 0
fi

scenes=$(find "$LAUNCHER_DIR" -name '*_render_host.sh' ! -path '*/build*' | sort)
if [ -z "$scenes" ]; then
    echo "No *_render_host.sh found under $LAUNCHER_DIR" >&2
    exit 1
fi

count=0
for scene in $scenes; do
    name=$(basename "$scene" _render_host.sh)
    echo "--- $name"
    if [ -n "$OUT_ROOT" ]; then
        sh "$scene" -o "$OUT_ROOT/$name"
    else
        sh "$scene"
    fi
    count=$((count + 1))
done

echo "$count scenes rendered, every image the size its scene declared"
