#!/bin/sh
# Sourced, not run: names the Linux driver's OptiX library, kept in ~/gpu/optix, that
# Dr.Jit loads under WSL 2 (README.md, "Reference on the GPU").
export DRJIT_LIBOPTIX_PATH=$HOME/gpu/optix/libnvoptix.so.1
export LD_LIBRARY_PATH=$HOME/gpu/optix${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
