#!/bin/sh
# Runs Python in the documented WSL CUDA toolchain of the appearance fit (README.md): the
# environment at GPU_ENV, ~/gpu/env by default, with its compiler and CUDA headers.
#   sh launcher/tools/r3d/gpu_python.sh SCRIPT [ARGS...]
set -eu
E=${GPU_ENV:-$HOME/gpu/env}
export CUDA_HOME=$E
export PATH=$E/bin:$PATH
export CPATH=$E/targets/x86_64-linux/include
export LIBRARY_PATH=$E/targets/x86_64-linux/lib:$E/lib
export CC=$E/bin/x86_64-conda-linux-gnu-gcc
export CXX=$E/bin/x86_64-conda-linux-gnu-g++
exec "$E/bin/python3" "$@"
