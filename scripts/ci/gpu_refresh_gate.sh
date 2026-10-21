#!/bin/sh
# Decides render=true|false inside the doc-images-gpu concurrency group: fails
# if this runner's fingerprint differs from the hosted FINGERPRINT, skips a run
# whose trigger SHA is no longer main's head. Skips the render only when main's
# newest unexpired record is a rendered one for this fingerprint; FORCE=true
# always renders. Writes GPU_ENV to
# GITHUB_ENV so later steps need not source gpu_env.sh (it puts conda's bin
# first on PATH). Run from the repository root.
set -eu
. launcher/tools/r3d/gpu_env.sh
fingerprint=$(sh launcher/tools/render/render_doc_images.sh --stage gpu --inputs)
if [ "$fingerprint" != "$FINGERPRINT" ]; then
    echo "GPU inputs $fingerprint differ from hosted inputs $FINGERPRINT" >&2
    exit 1
fi
echo "GPU_ENV=$GPU_ENV" >> "$GITHUB_ENV"
if ! current=$("$GPU_ENV/bin/python3" scripts/ci/inputs_recorded.py "$fingerprint" --skip-obsolete); then
    echo "GPU refresh lookup failed" >&2
    exit 1
fi
case "$current" in
    obsolete:*)
        echo "render=false" >> "$GITHUB_OUTPUT"
        echo "$current"
        exit 0
        ;;
esac
if [ "$current" = true ]; then
    echo "render=false" >> "$GITHUB_OUTPUT"
    echo "doc images current: latest run recorded inputs $fingerprint"
else
    echo "render=true" >> "$GITHUB_OUTPUT"
fi
