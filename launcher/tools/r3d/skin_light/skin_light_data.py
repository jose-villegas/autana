"""Write the skinned-lighting benchmark's input from a skinned glTF: every
vertex's bind normal, joints, weights and colour, and the rotation part of
every joint's skinning matrix at every frame of every animation, sampled at
FPS. The asset needs normals.

    python tools/r3d/skin_light/skin_light_data.py ASSET.glb OUT.bin [--sheet CLIP[:PHASE]]

--sheet names the frame the sheet draws: a clip and how far through it, 0 to
1; the first clip at 0.5 when left out. Run from launcher/. Standard library
only. The layout, little-endian, is what skin_light_bench.c reads:

    "SKLT", u32 vertices, u32 joints, u32 frames, u32 sheet frame
    per vertex: f32 normal[3], u8 joint[4], f32 weight[4], u8 sRGB colour[3], u8 0
    per frame, per joint: f32 rotation rows[3][3]
"""

import argparse
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from r3d import gltf_skin  # noqa: E402
from r3d.gltf_preview import linear_to_srgb  # noqa: E402

FPS = 30


def frame_count(asset, clip):
    return max(1, int(round(asset.duration(clip) * FPS)))


def frames(asset):
    """(clip, time) of every sampled frame, clips in file order."""
    out = []
    for name in asset.animations:
        count = frame_count(asset, name)
        out += [(name, asset.duration(name) * i / count) for i in range(count)]
    return out


def sheet_frame(asset, spec):
    """The sampled (clip, time) a --sheet value names, or the first clip's middle."""
    clip, _, phase = (spec or "").partition(":")
    clip = clip or next(iter(asset.animations))
    if clip not in asset.animations:
        raise SystemExit(f"no animation {clip!r}; the asset has {', '.join(asset.animations)}")
    count = frame_count(asset, clip)
    index = min(int(round(count * float(phase or 0.5))), count - 1)
    return clip, asset.duration(clip) * index / count


def sheet_argument(parser):
    parser.add_argument("--sheet", metavar="CLIP[:PHASE]",
                        help="the frame the sheet draws: a clip and how far through it, 0 to 1")


def write(asset, path, sheet):
    sampled = frames(asset)
    colours = asset.colors or [(0.6, 0.6, 0.6)] * len(asset.positions)
    out = bytearray(b"SKLT")
    out += struct.pack("<4I", len(asset.positions), len(asset.joints), len(sampled),
                       sampled.index(sheet_frame(asset, sheet)))
    for normal, joints, weights, colour in zip(asset.normals, asset.joint_indices, asset.weights, colours):
        srgb = [int(round(255 * linear_to_srgb(c))) for c in colour[:3]]
        out += struct.pack("<3f4B4f4B", *normal, *joints, *weights, *srgb, 0)
    for clip, time in sampled:
        for m in asset.joint_matrices(asset.sample(clip, time)):
            out += struct.pack("<9f", *m[0:3], *m[4:7], *m[8:11])
    pathlib.Path(path).write_bytes(bytes(out))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("asset")
    parser.add_argument("out")
    sheet_argument(parser)
    args = parser.parse_args()
    asset = gltf_skin.SkinnedAsset(*gltf_skin.load_asset(args.asset))
    if asset.normals is None:
        sys.exit(f"{args.asset}: the skinned mesh has no normals")
    write(asset, args.out, args.sheet)


if __name__ == "__main__":
    main()
