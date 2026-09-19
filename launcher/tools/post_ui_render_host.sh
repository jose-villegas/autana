#!/bin/sh
#
# Build post_ui_render_host.c and render the power-on self-test screen in
# every orientation worth looking at, so the layout can be judged without a
# flash cycle.
#
#   ./launcher/tools/post_ui_render_host.sh [-o <dir>]
#
# Writes into tools/results/post_ui_render/ unless -o says otherwise (that
# directory is gitignored, like every other tools/results output). Each BMP
# is checked for the dimensions it should have before the script exits, so a
# renderer that has quietly stopped working is a failure here rather than a
# picture nobody looks at twice. A .png is written beside each BMP when
# Pillow happens to be installed; it is not required.
#
# The binary is built the same way boot_anim_render_host.c is - find_cc.sh
# for a host compiler, the real firmware translation units, `main` and the
# vendored microui headers on the include path. post.c is NOT among them;
# see the tool's own top comment for what stands in for it.
#
# POSIX sh, like the rest of this directory.

set -eu

TOOLS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MAIN_DIR=$(CDPATH= cd -- "$TOOLS_DIR/../main" && pwd)
MICROUI_DIR=$(CDPATH= cd -- "$TOOLS_DIR/../components/microui/include" && pwd)

OUT_DIR="$TOOLS_DIR/results/post_ui_render"
while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT_DIR="$2"; shift 2 ;;
        *) echo "usage: $0 [-o <dir>]" >&2; exit 2 ;;
    esac
done

# shellcheck source=./find_cc.sh
. "$TOOLS_DIR/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Git Bash hands the compiler and this script MSYS paths (/c/...), which the
# Windows python the checker runs under cannot open. cygpath exists only
# there, which is also the only place the conversion is needed.
to_native() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

mkdir -p "$OUT_DIR"
BIN="$OUT_DIR/post_ui_render_host"
if [ "${OS:-}" = "Windows_NT" ]; then
    BIN="$BIN.exe"
fi

"$CC_BIN" -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -Wno-unused-variable -O1 \
    -I "$MAIN_DIR" -I "$MICROUI_DIR" \
    "$TOOLS_DIR/post_ui_render_host.c" \
    "$MAIN_DIR/gfx/gfx.c" \
    "$MAIN_DIR/boot/post_ui.c" \
    "$MAIN_DIR/boot/post_layout.c" \
    -o "$BIN"

# Landscape is the shipping orientation, so it leads: once as it is read,
# once as the panel holds it (the shape a device screenshot has).
"$BIN" 1 > "$OUT_DIR/landscape.bmp"
"$BIN" 1 --panel > "$OUT_DIR/landscape-panel.bmp"
"$BIN" 0 > "$OUT_DIR/portrait.bmp"
"$BIN" 1 --failures > "$OUT_DIR/landscape-fault.bmp"

python "$(to_native "$TOOLS_DIR/post_ui_render_check.py")" \
    "$(to_native "$OUT_DIR/landscape.bmp"):448:368" \
    "$(to_native "$OUT_DIR/landscape-panel.bmp"):368:448" \
    "$(to_native "$OUT_DIR/portrait.bmp"):368:448" \
    "$(to_native "$OUT_DIR/landscape-fault.bmp"):448:368"

echo "wrote $OUT_DIR"
