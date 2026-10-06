"""Write the skinned-lighting benchmark's input from a skinned glTF: every
vertex's bind normal, joints, weights and colour, and the rotation part of
every joint's skinning matrix at every frame of every animation, sampled at
FPS.

    python tools/r3d/skin_light/skin_light_data.py ASSET.glb OUT.bin

Run from launcher/. Standard library only. The layout, little-endian, is what
skin_light_bench.c reads:

    "SKLT", u32 vertices, u32 joints, u32 frames, u32 sheet frame
    per vertex: f32 normal[3], u8 joint[4], f32 weight[4], u8 sRGB colour[3], u8 0
    per frame, per joint: f32 rotation rows[3][3]
"""

import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from r3d import gltf_skin  # noqa: E402
from r3d.gltf_preview import linear_to_srgb  # noqa: E402

FPS = 30
# The pose the sheet draws: mid-stride of the fastest gait, legs spread.
SHEET_CLIP, SHEET_PHASE = "gallop", 0.5


def frames(asset):
    """(clip, time) of every sampled frame, clips in file order."""
    out = []
    for name in asset.animations:
        duration = asset.duration(name)
        count = max(1, int(round(duration * FPS)))
        out += [(name, duration * i / count) for i in range(count)]
    return out


def sheet_frame(asset):
    duration = asset.duration(SHEET_CLIP)
    return SHEET_CLIP, duration * int(round(duration * FPS * SHEET_PHASE)) / int(round(duration * FPS))


def write(asset, path):
    sampled = frames(asset)
    colours = asset.colors or [(0.6, 0.6, 0.6)] * len(asset.positions)
    out = bytearray(b"SKLT")
    out += struct.pack("<4I", len(asset.positions), len(asset.joints), len(sampled),
                       sampled.index(sheet_frame(asset)))
    for normal, joints, weights, colour in zip(asset.normals, asset.joint_indices, asset.weights, colours):
        srgb = [int(round(255 * linear_to_srgb(c))) for c in colour[:3]]
        out += struct.pack("<3f4B4f4B", *normal, *joints, *weights, *srgb, 0)
    for clip, time in sampled:
        for m in asset.joint_matrices(asset.sample(clip, time)):
            out += struct.pack("<9f", *m[0:3], *m[4:7], *m[8:11])
    pathlib.Path(path).write_bytes(bytes(out))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    write(gltf_skin.SkinnedAsset(*gltf_skin.load_glb(sys.argv[1])), sys.argv[2])


if __name__ == "__main__":
    main()
