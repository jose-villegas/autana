"""Offline mesh baking for the firmware's software renderers: load, decimate,
split for baked light, light, and group into a cluster tree. Host only; see
README.md for the environment."""

import sys


def log(*args):
    print(*args, file=sys.stderr, flush=True)
