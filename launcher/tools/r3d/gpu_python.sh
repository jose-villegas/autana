#!/bin/sh
# Runs Python in the documented WSL CUDA toolchain of the appearance fit (README.md): the
# environment at GPU_ENV, ~/gpu/env by default, with its compiler and CUDA headers.
#   sh launcher/tools/r3d/gpu_python.sh SCRIPT [ARGS...]
set -eu
. "$(dirname -- "$0")/gpu_env.sh"
exec "$E/bin/python3" "$@"
