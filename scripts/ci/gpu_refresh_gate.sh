#!/bin/sh
# The concurrency group serializes the latest-record check with rendering.
set -eu
. launcher/tools/r3d/gpu_env.sh
fingerprint=$(sh launcher/tools/render/render_doc_images.sh --stage gpu --inputs)
if [ "$fingerprint" != "$FINGERPRINT" ]; then
    echo "GPU inputs $fingerprint differ from hosted inputs $FINGERPRINT" >&2
    exit 1
fi
echo "GPU_ENV=$GPU_ENV" >> "$GITHUB_ENV"
current=$("$GPU_ENV/bin/python3" scripts/ci/inputs_recorded.py "$fingerprint")
if [ "$current" = true ]; then
    echo "render=false" >> "$GITHUB_OUTPUT"
    echo "doc images current: latest run recorded inputs $fingerprint"
else
    echo "render=true" >> "$GITHUB_OUTPUT"
fi
