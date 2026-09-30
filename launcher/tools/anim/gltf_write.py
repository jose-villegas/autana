"""Write a binary glTF 2.0 holding nodes, cameras and animations.

Standard library only. It writes what tools/anim/bake_tracks.py reads: node
TRS, an optional camera, and animation channels on translation, rotation,
scale or, through KHR_animation_pointer, any property path. A converter from
another animation format and a test that needs a scene of its own both use it.
"""

import json
import struct

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
FLOAT = 5126
WIDTH_NAMES = {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4"}


def _pad(data, fill):
    return data + fill * (-len(data) % 4)


def build_glb(nodes, animations, cameras=()):
    """`nodes`: dicts with optional name, translation, rotation, scale, camera.
    `cameras`: glTF camera dicts. `animations`: dicts with a name and channels,
    each channel a dict with `node` (None for a pointer channel), `path`,
    `pointer`, `interpolation`, `times` and `values`: one tuple per key, or
    three per key (in-tangent, value, out-tangent) for CUBICSPLINE. Returns
    the file's bytes."""
    blob = bytearray()
    views, accessors = [], []

    def accessor(rows, width, extra=None):
        data = b"".join(struct.pack("<%df" % width, *row) for row in rows)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob.extend(_pad(data, b"\0"))
        entry = {"bufferView": len(views) - 1, "componentType": FLOAT,
                 "type": WIDTH_NAMES[width], "count": len(rows)}
        entry.update(extra or {})
        accessors.append(entry)
        return len(accessors) - 1

    out_animations = []
    pointers = False
    for animation in animations:
        samplers, channels = [], []
        for channel in animation["channels"]:
            times = channel["times"]
            width = len(channel["values"][0])
            samplers.append({
                "input": accessor([(t,) for t in times], 1,
                                  {"min": [min(times)], "max": [max(times)]}),
                "output": accessor(channel["values"], width),
                "interpolation": channel.get("interpolation", "LINEAR"),
            })
            target = {"path": channel["path"]}
            if channel.get("node") is not None:
                target["node"] = channel["node"]
            if channel.get("pointer"):
                pointers = True
                target["extensions"] = {"KHR_animation_pointer": {"pointer": channel["pointer"]}}
            channels.append({"sampler": len(samplers) - 1, "target": target})
        out_animations.append({"name": animation.get("name", ""),
                               "samplers": samplers, "channels": channels})

    document = {
        "asset": {"version": "2.0", "generator": "autana tools/anim/gltf_write.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": list(nodes),
        "animations": out_animations,
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    if cameras:
        document["cameras"] = list(cameras)
    if pointers:
        document["extensionsUsed"] = ["KHR_animation_pointer"]
    text = _pad(json.dumps(document, separators=(",", ":")).encode("utf-8"), b" ")
    binary = _pad(bytes(blob), b"\0")
    total = 12 + 8 + len(text) + 8 + len(binary)
    return (struct.pack("<III", GLB_MAGIC, 2, total)
            + struct.pack("<II", len(text), CHUNK_JSON) + text
            + struct.pack("<II", len(binary), CHUNK_BIN) + binary)
