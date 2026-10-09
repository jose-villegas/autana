"""FBX source support: an FBX is converted to a plain glTF binary, the one
intermediate every mesh and animation tool reads (tools/gltf/gltf_read.py)."""

import sys


def log(*args):
    print(*args, file=sys.stderr, flush=True)
