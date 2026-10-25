#!/bin/sh
# Run a command in the pinned CI image or the developer's ESP-IDF environment.
set -eu

workdir=$1
command=$2
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
if [ "${CI:-}" = true ]; then
    image=$(sh scripts/ci/idf_image.sh)
    set -- --rm -v "$GITHUB_WORKSPACE:/app" -w "/app/$workdir"
    if [ -n "${CCACHE_MAXSIZE:-}" ]; then
        set -- "$@" -t -e IDF_TARGET=esp32s3 -v "$RUNNER_TEMP/bakes:/bakes" -e AUTANA_BAKE_CACHE=/bakes \
            -v "$RUNNER_TEMP/ccache:/ccache" -e CCACHE_DIR=/ccache -e CCACHE_MAXSIZE
        container_command='git config --global --add safe.directory "*" && ccache -z > /dev/null && eval "$BUILD_COMMAND" && ccache -s'
    else
        container_command='git config --global --add safe.directory /app && eval "$BUILD_COMMAND"'
    fi
    BUILD_COMMAND=$command export BUILD_COMMAND
    docker run "$@" -e BUILD_COMMAND "$image" /bin/bash -c \
        "$container_command"
else
    if [ -n "${CCACHE_MAXSIZE:-}" ]; then
        export AUTANA_BAKE_CACHE="$RUNNER_TEMP/bakes"
    fi
    . "$root/launcher/tools/build/idf.sh"
    idf_init "$root/launcher" "$root/launcher/tools/build"
    idf_in "$root/$workdir" bash --noprofile --norc -c "$command"
fi
