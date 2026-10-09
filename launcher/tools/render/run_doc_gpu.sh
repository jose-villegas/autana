#!/bin/sh
# The GPU doc stage, in the appearance fit's CUDA toolchain (r3d/gpu_python.sh).
set -eu
exec sh "$(dirname "$0")/../r3d/gpu_python.sh" launcher/tools/render/doc_stages.py --stage gpu "$@"
