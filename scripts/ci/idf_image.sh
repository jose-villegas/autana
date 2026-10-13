#!/bin/sh
#
# Prints the reference a CI job runs the pinned ESP-IDF image by: the GHCR
# mirror the ESP-IDF refresh workflow keeps (the same digest, copied byte for
# byte), else Docker Hub's, whose anonymous pull quota is what the mirror is
# for. Either way the digest is launcher/idf-pin.json's.
#
#   image=$(sh scripts/ci/idf_image.sh)
#
# Run from the repository root, with GITHUB_REPOSITORY_OWNER set (Actions sets it).

set -eu

image=$(python3 scripts/deps/idf_pin.py show image)
mirror="ghcr.io/$GITHUB_REPOSITORY_OWNER/esp-idf@${image#*@}"
if docker manifest inspect "$mirror" >/dev/null 2>&1; then
    echo "$mirror"
else
    echo "::warning::$mirror is not mirrored; pulling $image from Docker Hub. The ESP-IDF refresh workflow mirrors it." >&2
    echo "$image"
fi
