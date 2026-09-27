"""Offline mesh baking for the r3d renderer (main/render/): load, decimate,
split for baked light, light, and group into a cluster tree. Host only; see
README.md for the environment."""

import sys


def log(*args):
    print(*args, file=sys.stderr, flush=True)
