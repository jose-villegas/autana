#!/bin/sh
#
# The renderer's sources, for a host build that compiles it without CMake:
# every .c below the folders main/renderer_folders.txt names, relative to
# launcher/ and sorted. renderer.py is the Python twin.
#
# Usage: source this file and pass the launcher directory:
#
#   . "$LAUNCHER_DIR/tools/build/renderer.sh"
#   for source in $(renderer_sources "$LAUNCHER_DIR"); do ...

renderer_sources() {
    (
        cd "$1/main" || exit 1
        # shellcheck disable=SC2046 # one folder name per line
        find $(grep -v '^#' renderer_folders.txt) -name '*.c' | LC_ALL=C sort | sed 's@^@main/@'
    )
}
