#!/bin/sh
# The documented WSL CUDA toolchain for the GPU doc stage.
set -eu
E=${GPU_ENV:-$HOME/gpu/env}
export CUDA_HOME=$E
export PATH=$E/bin:$PATH
export CPATH=$E/targets/x86_64-linux/include
export LIBRARY_PATH=$E/targets/x86_64-linux/lib:$E/lib
export CC=$E/bin/x86_64-conda-linux-gnu-gcc
export CXX=$E/bin/x86_64-conda-linux-gnu-g++
exec "$E/bin/python3" launcher/tools/render/doc_stages.py --stage gpu "$@"
