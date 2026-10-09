#!/bin/sh
#
# Compiles the sand simulation and web_sand.c to WebAssembly, into dist/
# beside index.html, app.js and style.css. dist/ is a static site: any file
# server can host it, with nothing running server-side.
#
#   launcher/main/apps/sand/tools/web/setup_emsdk.sh    (once, unless emcc is on PATH)
#   launcher/main/apps/sand/tools/web/build_web.sh [<out dir>]
#   launcher/main/apps/sand/tools/web/build_web.sh --inputs
#
# --inputs lists, one repository path per line, every file the build reads:
# the sources below, the headers they include (as a host cc sees them, $CC or
# cc), and the files tracked in this directory. It needs no emcc.
#
# POSIX sh, like the host test runner, so it runs under Git Bash on Windows.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SAND_DIR/../.." && pwd)
# The simulation and what it links against. The painter, brush list and
# paint clocks are headers, so they need no entry here.
SOURCES="
$SAND_DIR/material.c
$SAND_DIR/material_palette.c
$SAND_DIR/sand.c
$SAND_DIR/sand_chunk_sched.c
$SAND_DIR/sand_impulse.c
$SAND_DIR/sand_liquid.c
$SAND_DIR/sand_gas.c
$SAND_DIR/sand_reactions.c
$SAND_DIR/sand_plants.c
$MAIN_DIR/input/tilt.c
$MAIN_DIR/util/runtime/job.c
$SCRIPT_DIR/web_sand.c
"

FLAGS="-std=c11 -DNDEBUG -I $MAIN_DIR -I $SAND_DIR"

if [ "${1:-}" = --inputs ]; then
    ROOT=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)
    # -MG: emcc's own headers are missing here, and are not the repository's.
    # shellcheck disable=SC2086
    DEPS=$(${CC:-cc} -MM -MG -D__EMSCRIPTEN__ $FLAGS $SOURCES |
        sed 's/ *\\$//' | tr -s ' ' '\n' | sed -n "s|^$ROOT/||p")
    # Every source must come out as a repository path, or the list is wrong.
    # The compiler may spell a path another way than $SOURCES (C:/ for /c/),
    # so this counts them.
    LISTED=$(printf '%s\n' "$DEPS" | grep -c '\.c$' || true)
    # shellcheck disable=SC2086
    if [ "$LISTED" -ne "$(printf '%s\n' $SOURCES | wc -l)" ]; then
        echo "--inputs: $LISTED of the sources came out under $ROOT" >&2
        exit 1
    fi
    {
        printf '%s\n' "$DEPS"
        git -C "$ROOT" ls-files --full-name "$SCRIPT_DIR"
    } | sort -u
    exit 0
fi

DIST_DIR=${1:-$SCRIPT_DIR/dist}

if ! command -v emcc >/dev/null 2>&1 && [ -f "$SCRIPT_DIR/emsdk/emsdk_env.sh" ]; then
    # shellcheck source=/dev/null
    . "$SCRIPT_DIR/emsdk/emsdk_env.sh" >/dev/null
fi
if ! command -v emcc >/dev/null 2>&1; then
    echo "emcc not found: run $SCRIPT_DIR/setup_emsdk.sh first" >&2
    exit 1
fi

mkdir -p "$DIST_DIR"

# The host test runner's warnings, reported but not fatal: emsdk "latest"
# is clang, whose new diagnostics should not block a deploy. MODULARIZE
# gives app.js a SandModule() promise;
# cwrap and HEAPU8 let it call the web_* exports by name and read the frame
# without a copy.
# shellcheck disable=SC2086
emcc -O3 $FLAGS -Wall -Wextra -Wno-unused-parameter \
    $SOURCES \
    -o "$DIST_DIR/sand.js" \
    -s MODULARIZE=1 \
    -s EXPORT_NAME=SandModule \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORTED_RUNTIME_METHODS='["cwrap","HEAPU8"]' \
    -s ENVIRONMENT=web

cp "$SCRIPT_DIR/app.js" "$SCRIPT_DIR/style.css" "$DIST_DIR/"

# The three references carry the commit, so a browser holding an older
# deploy's app.js never pairs it with a newer sand.wasm: the two must agree
# on every web_* signature.
VERSION=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || date +%s)
sed -e "s/sand\.js\"/sand.js?v=$VERSION\"/" \
    -e "s/app\.js\"/app.js?v=$VERSION\"/" \
    -e "s/style\.css\"/style.css?v=$VERSION\"/" \
    "$SCRIPT_DIR/index.html" >"$DIST_DIR/index.html"

echo "built $DIST_DIR (v=$VERSION)"
