"""Offline mesh baking for the r3d renderer (main/render/): load, simplify,
split for baked light, light, group into a cluster tree and write it as C
data. Host only; see README.md for the environment."""

import sys


def log(*args):
    print(*args, file=sys.stderr, flush=True)
