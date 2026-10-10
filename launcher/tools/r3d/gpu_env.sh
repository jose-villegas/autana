#!/bin/sh
# Sourced, not run: exports GPU_ENV (~/gpu/env by default) with its compiler, CUDA
# headers and libraries, and its bin first on PATH.
export GPU_ENV=${GPU_ENV:-$HOME/gpu/env}
export CUDA_HOME=$GPU_ENV
export PATH=$GPU_ENV/bin:$PATH
export CPATH=$GPU_ENV/targets/x86_64-linux/include
export LIBRARY_PATH=$GPU_ENV/targets/x86_64-linux/lib:$GPU_ENV/lib
export CC=$GPU_ENV/bin/x86_64-conda-linux-gnu-gcc
export CXX=$GPU_ENV/bin/x86_64-conda-linux-gnu-g++
